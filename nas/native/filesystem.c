// SPDX-License-Identifier: Apache-2.0
#include "netfs.h"
#include "deadline.h"
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <limits.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/socket.h>
#include <unistd.h>

struct nf_file {
    struct nf_file *next;
    void *remote;
    char *path, *cancel_path;
    int flags, cache_fd, dirty, error;
    char cache_path[PATH_MAX], manifest[PATH_MAX];
    unsigned char *write_buffer, *read_buffer;
    size_t write_count, read_count;
    uint64_t write_offset, read_offset, size;
    uint64_t uploaded;
    int uploading;
};
struct filesystem {
    struct nf_config *config;
    struct nf_remote remote;
    const struct nf_ops *ops;
    const struct nf_ops *backend;
    struct nf_deadline deadline;
    int offline;
    struct stat root;
    struct statvfs space;
    struct nf_file *files;
    uint64_t written, read, write_errors, retained, retained_files;
    size_t memory;
    double status_time;
    char status_temp[PATH_MAX];
} fs;
static pthread_mutex_t remote_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t space_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t maintenance_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t maintenance_changed=PTHREAD_COND_INITIALIZER;
static int maintenance_stop;

static int healthy(void) {
    if (fs.offline) return -ENOTCONN;
    return (!strcmp(fs.config->type,"smb") ? fs.remote.smb!=NULL : fs.remote.sftp!=NULL) ? 0 : -ENOTCONN;
}
static void status_update(int force) {
    if (!*fs.config->status_file || (!force && nf_now()-fs.status_time<0.25)) return;
    fs.status_time=nf_now();
    uint64_t pending=fs.retained,files=fs.retained_files,uploading=0,uploaded=0,total=0;
    for (struct nf_file *f=fs.files;f;f=f->next) {
        if (f->dirty) {
            pending+=f->cache_fd>=0 ? f->size-(f->uploading ? f->uploaded : 0) : f->write_count;
            ++files;
        }
        if (f->uploading) { ++uploading;uploaded+=f->uploaded;total+=f->size; }
    }
    struct json_object *root=json_object_new_object();
    json_object_object_add(root,"version",json_object_new_int(1));
    json_object_object_add(root,"pid",json_object_new_int(getpid()));
    json_object_object_add(root,"written",json_object_new_uint64(fs.written));
    json_object_object_add(root,"read",json_object_new_uint64(fs.read));
    json_object_object_add(root,"pending_bytes",json_object_new_uint64(pending));
    json_object_object_add(root,"pending_files",json_object_new_uint64(files));
    json_object_object_add(root,"uploading_files",json_object_new_uint64(uploading));
    json_object_object_add(root,"uploading_bytes",json_object_new_uint64(uploaded));
    json_object_object_add(root,"uploading_total",json_object_new_uint64(total));
    json_object_object_add(root,"retained_files",json_object_new_uint64(fs.retained_files));
    json_object_object_add(root,"write_errors",json_object_new_uint64(fs.write_errors));
    json_object_object_add(root,"connected",json_object_new_boolean(healthy()==0));
    json_object_object_add(root,"disk_cache",json_object_new_boolean(*fs.config->cache_dir!=0));
    const char *data=json_object_to_json_string_ext(root,JSON_C_TO_STRING_PLAIN);
    int fd=open(fs.status_temp,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);
    if (fd>=0) {
        size_t size=strlen(data),done=0;
        while (done<size) {
            ssize_t n=write(fd,data+done,size-done);
            if (n<0 && errno==EINTR) continue;
            if (n<=0) break;
            done+=(size_t)n;
        }
        if (close(fd)==0 && done==size) rename(fs.status_temp,fs.config->status_file);
        else unlink(fs.status_temp);
    }
    json_object_put(root);
}

/* All protocol contexts and file buffers remain owned by one FUSE worker at a time. */
#define REMOTE_CALL(name,params,args,timeout) \
static int timed_##name params { \
    if (healthy()) return -ENOTCONN; \
    int rc=nf_deadline_begin(&fs.deadline,fs.backend->socket_fd(r),timeout); \
    if (rc) return rc; \
    rc=fs.backend->name args; \
    int lost=nf_deadline_end(&fs.deadline); \
    if (!lost && (healthy() || rc==-ETIMEDOUT || rc==-ECONNRESET || rc==-EPIPE || \
        rc==-ENETUNREACH || rc==-EHOSTUNREACH || rc==-ENOTCONN || rc==-ECONNABORTED)) lost=-ENOTCONN; \
    if (lost) { \
        fs.offline=1;nf_log(fs.config,"NAS connection lost; reconnect to resume access",lost);status_update(1); \
        return rc<0 ? rc : lost; \
    } \
    return rc; \
}
REMOTE_CALL(stat,(struct nf_remote *r,const char *p,struct stat *s,void *f),(r,p,s,f),5000)
REMOTE_CALL(list,(struct nf_remote *r,const char *p,nf_entry_fn e,void *d),(r,p,e,d),5000)
REMOTE_CALL(open,(struct nf_remote *r,const char *p,int flags,mode_t mode,void **f),(r,p,flags,mode,f),5000)
REMOTE_CALL(close,(struct nf_remote *r,void *f),(r,f),5000)
REMOTE_CALL(read,(struct nf_remote *r,void *f,void *b,size_t n,uint64_t o),(r,f,b,n,o),30000)
REMOTE_CALL(write,(struct nf_remote *r,void *f,const void *b,size_t n,uint64_t o),(r,f,b,n,o),30000)
REMOTE_CALL(flush,(struct nf_remote *r,void *f),(r,f),30000)
REMOTE_CALL(truncate,(struct nf_remote *r,const char *p,void *f,uint64_t n),(r,p,f,n),5000)
REMOTE_CALL(mkdir,(struct nf_remote *r,const char *p,mode_t m),(r,p,m),5000)
REMOTE_CALL(unlink,(struct nf_remote *r,const char *p,int d),(r,p,d),5000)
REMOTE_CALL(rename,(struct nf_remote *r,const char *p,const char *q,unsigned flags),(r,p,q,flags),5000)
REMOTE_CALL(chmod,(struct nf_remote *r,const char *p,mode_t m),(r,p,m),5000)
REMOTE_CALL(chown,(struct nf_remote *r,const char *p,uid_t u,gid_t g),(r,p,u,g),5000)
REMOTE_CALL(utimens,(struct nf_remote *r,const char *p,const struct timespec t[2]),(r,p,t),5000)
REMOTE_CALL(statfs,(struct nf_remote *r,const char *p,struct statvfs *s),(r,p,s),5000)
REMOTE_CALL(readlink,(struct nf_remote *r,const char *p,char *b,size_t n),(r,p,b,n),5000)
REMOTE_CALL(symlink,(struct nf_remote *r,const char *p,const char *q),(r,p,q),5000)
#undef REMOTE_CALL
static const struct nf_ops timed_ops={
    .stat=timed_stat,.list=timed_list,.open=timed_open,.close=timed_close,
    .read=timed_read,.write=timed_write,.flush=timed_flush,.truncate=timed_truncate,
    .mkdir=timed_mkdir,.unlink=timed_unlink,.rename=timed_rename,.chmod=timed_chmod,
    .chown=timed_chown,.utimens=timed_utimens,.statfs=timed_statfs,
    .readlink=timed_readlink,.symlink=timed_symlink
};
static int file_error(struct nf_file *f,int rc) {
    if (rc>=0) rc=-EIO;
    if (!f->error) {
        f->error=rc;
        ++fs.write_errors;
        nf_log(fs.config,"Remote file write/flush failed",rc);
    }
    status_update(1);
    return f->error;
}
static unsigned char *allocate_buffer(void) {
    /* File sessions are serialized, so idle readahead buffers can be evicted. */
    for (struct nf_file *f=fs.files;fs.memory+NF_WINDOW>64U*NF_BLOCK && f;f=f->next) {
        if (f->read_buffer) {
            free(f->read_buffer);f->read_buffer=NULL;f->read_count=0;fs.memory-=NF_WINDOW;
        }
        if (fs.memory+NF_WINDOW>64U*NF_BLOCK && f->write_buffer && !f->write_count) {
            free(f->write_buffer);f->write_buffer=NULL;fs.memory-=NF_WINDOW;
        }
    }
    if (fs.memory+NF_WINDOW>64U*NF_BLOCK) return NULL;
    unsigned char *buffer=malloc(NF_WINDOW);
    if (buffer) fs.memory+=NF_WINDOW;
    return buffer;
}
static void free_buffer(unsigned char **buffer) {
    if (*buffer) { free(*buffer);*buffer=NULL;fs.memory-=NF_WINDOW; }
}
static void account_space(uint64_t written) {
    pthread_mutex_lock(&space_mutex);
    uint64_t unit=fs.space.f_frsize ? fs.space.f_frsize : fs.space.f_bsize;
    if (unit) {
        /* Stay conservative between asynchronous free-space refreshes. */
        uint64_t blocks=(written+unit-1)/unit;
        fs.space.f_bfree=fs.space.f_bfree>blocks ? fs.space.f_bfree-blocks : 0;
        fs.space.f_bavail=fs.space.f_bavail>blocks ? fs.space.f_bavail-blocks : 0;
    }
    pthread_mutex_unlock(&space_mutex);
}
static int upload_cancelled(const struct nf_file *f) {
    char path[PATH_MAX],scope[PATH_MAX];
    if (!*fs.config->status_file ||
        snprintf(path,sizeof(path),"%s.cancel",fs.config->status_file)>=(int)sizeof(path)) return 0;
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if (fd<0) return 0;
    ssize_t n=read(fd,scope,sizeof(scope)-1);close(fd);
    if (n<=0) return 0;
    scope[n]=0;scope[strcspn(scope,"\r\n")]=0;
    size_t length=strlen(scope);
    return length && scope[0]=='/' && !strncmp(f->cancel_path,scope,length) &&
        (length==1 || f->cancel_path[length]==0 || f->cancel_path[length]=='/');
}
static int drain(struct nf_file *f) {
    if (f->error) return f->error;
    if (!f->write_count) return 0;
    if (upload_cancelled(f)) return file_error(f,-ECANCELED);
    int rc=healthy();
    if (!rc) rc=fs.ops->write(&fs.remote,f->remote,f->write_buffer,f->write_count,f->write_offset);
    if (rc<0 || (size_t)rc!=f->write_count) return file_error(f,rc);
    fs.written+=f->write_count;
    account_space(f->write_count);
    f->write_count=0;
    status_update(0);
    return 0;
}
static int flush_file(struct nf_file *f) {
    if (f->error) return f->error;
    if (!f->dirty) return 0;
    if (healthy()) return file_error(f,-ENOTCONN);
    int rc=0;
    if (f->cache_fd>=0) {
        if (upload_cancelled(f)) return file_error(f,-ECANCELED);
        f->uploading=1;f->uploaded=0;status_update(1);
        unsigned char *buffer=allocate_buffer();
        if (!buffer) { f->uploading=0;return file_error(f,-ENOMEM); }
        if (fsync(f->cache_fd)) rc=-errno;
        if (!rc) rc=fs.ops->truncate(&fs.remote,f->path,f->remote,0);
        for (uint64_t offset=0;!rc && offset<f->size;) {
            if (upload_cancelled(f)) { rc=-ECANCELED;break; }
            size_t count=f->size-offset>NF_WINDOW ? NF_WINDOW : (size_t)(f->size-offset);
            ssize_t n=pread(f->cache_fd,buffer,count,(off_t)offset);
            if (n<0 && errno==EINTR) continue;
            if (n<=0) { rc=-EIO;break; }
            int sent=fs.ops->write(&fs.remote,f->remote,buffer,(size_t)n,offset);
            if (sent!=n) { rc=sent<0 ? sent : -EIO;break; }
            offset+=(uint64_t)n;
            f->uploaded=offset;
            fs.written+=(uint64_t)n;
            account_space((uint64_t)n);
            status_update(0);
        }
        free_buffer(&buffer);
    } else rc=drain(f);
    status_update(1);
    if (!rc) rc=fs.ops->flush(&fs.remote,f->remote);
    if (!rc) {
        struct stat st;
        rc=fs.ops->stat(&fs.remote,f->path,&st,f->remote);
        if (!rc && (uint64_t)st.st_size!=f->size) rc=-EIO;
    }
    f->uploading=0;
    if (rc) return file_error(f,rc);
    f->dirty=0;
    free_buffer(&f->write_buffer);
    status_update(1);
    return 0;
}
static int cache_open(struct nf_file *f) {
    if (snprintf(f->cache_path,sizeof(f->cache_path),"%s/.aera-netfs-XXXXXX",fs.config->cache_dir)>=(int)sizeof(f->cache_path)) return -ENAMETOOLONG;
    f->cache_fd=mkstemp(f->cache_path);
    if (f->cache_fd<0) return -errno;
    fcntl(f->cache_fd,F_SETFD,FD_CLOEXEC);
    if (snprintf(f->manifest,sizeof(f->manifest),"%s.json",f->cache_path)>=(int)sizeof(f->manifest)) return -ENAMETOOLONG;
    struct json_object *root=json_object_new_object();
    json_object_object_add(root,"version",json_object_new_int(1));
    json_object_object_add(root,"remote_path",json_object_new_string(f->path));
    json_object_object_add(root,"host",json_object_new_string(fs.config->host));
    json_object_object_add(root,"share",json_object_new_string(fs.config->share));
    json_object_object_add(root,"type",json_object_new_string(fs.config->type));
    int fd=open(f->manifest,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    int rc=0;
    const char *json=json_object_to_json_string_ext(root,JSON_C_TO_STRING_PLAIN);
    if (fd<0) rc=-errno;
    else {
        size_t n=strlen(json);
        if (write(fd,json,n)!=(ssize_t)n || fsync(fd)) rc=-EIO;
        close(fd);
    }
    json_object_put(root);
    if (rc || !f->size) return rc;
    if (f->size>25ULL*1024*NF_BLOCK) return -EFBIG;
    void *reader=NULL;
    rc=fs.ops->open(&fs.remote,f->path,O_RDONLY,0,&reader);
    if (rc) return rc;
    unsigned char *buffer=allocate_buffer();
    if (!buffer) rc=-ENOMEM;
    for (uint64_t offset=0;!rc && offset<f->size;) {
        size_t count=f->size-offset>NF_WINDOW ? NF_WINDOW : (size_t)(f->size-offset);
        int n=fs.ops->read(&fs.remote,reader,buffer,count,offset);
        if (n<=0 || pwrite(f->cache_fd,buffer,(size_t)n,(off_t)offset)!=n) { rc=n<0 ? n : -EIO;break; }
        offset+=(uint64_t)n;
    }
    free_buffer(&buffer);
    int closed=fs.ops->close(&fs.remote,reader);
    return rc ? rc : closed;
}
static int open_file(const char *path,mode_t mode,struct fuse_file_info *info,int create) {
    if (healthy()) return -ENOTCONN;
    struct nf_file *f=calloc(1,sizeof(*f));
    if (!f) return -ENOMEM;
    f->cache_fd=-1;
    char remote[8192]={0};
    int rc=nf_path(fs.config,path,remote,sizeof(remote));
    f->path=strdup(remote);
    f->cancel_path=strdup(path);
    f->flags=info->flags|(create ? O_CREAT : 0);
    if (!f->path || !f->cancel_path) rc=-ENOMEM;
    if (!rc) rc=fs.ops->open(&fs.remote,f->path,f->flags,mode,&f->remote);
    if (!rc) {
        struct stat st;
        rc=fs.ops->stat(&fs.remote,f->path,&st,f->remote);
        if (!rc) f->size=st.st_size;
    }
    int writable=(f->flags&O_ACCMODE)!=O_RDONLY;
    if (!rc && writable && *fs.config->cache_dir) rc=cache_open(f);
    if (rc) {
        if (f->remote) fs.ops->close(&fs.remote,f->remote);
        if (f->cache_fd>=0) close(f->cache_fd);
        if (*f->cache_path) unlink(f->cache_path);
        if (*f->manifest) unlink(f->manifest);
        free(f->path);free(f->cancel_path);free(f);return rc;
    }
    f->dirty=writable && (f->flags&(O_CREAT|O_TRUNC));
    f->next=fs.files;fs.files=f;
    info->fh=(uint64_t)(uintptr_t)f;
    info->direct_io=1;
    info->keep_cache=0;
    status_update(1);
    return 0;
}
static int nf_open(const char *p,struct fuse_file_info *f) { return open_file(p,0666,f,0); }
static int nf_create(const char *p,mode_t mode,struct fuse_file_info *f) { return open_file(p,mode,f,1); }
static int nf_getattr(const char *p,struct stat *st,struct fuse_file_info *info) {
    if (healthy()) return -ENOTCONN;
    char path[8192];int rc=nf_path(fs.config,p,path,sizeof(path));
    struct nf_file *f=info ? (void *)(uintptr_t)info->fh : NULL;
    if (!rc) rc=fs.ops->stat(&fs.remote,path,st,f ? f->remote : NULL);
    if (!rc) for (struct nf_file *open=fs.files;open;open=open->next) {
        if (!strcmp(open->path,path) && open->dirty) { st->st_size=open->size;st->st_blocks=(open->size+511)/512;break; }
    }
    return rc;
}
struct listing { void *buffer;fuse_fill_dir_t fill;off_t skip,index;int full;enum fuse_fill_dir_flags flags; };
static int emit(void *opaque,const char *name,const struct stat *st) {
    struct listing *l=opaque;
    ++l->index;
    if (l->index<=l->skip || l->full) return 0;
    l->full=l->fill(l->buffer,name,st,l->index,l->flags)!=0;
    return 0;
}
static int nf_readdir(const char *p,void *buffer,fuse_fill_dir_t fill,off_t offset,struct fuse_file_info *info,enum fuse_readdir_flags flags) {
    (void)info;
    if (healthy()) return -ENOTCONN;
    char path[8192];int rc=nf_path(fs.config,p,path,sizeof(path));
    struct listing l={.buffer=buffer,.fill=fill,.skip=offset,
        .flags=(flags&FUSE_READDIR_PLUS) ? FUSE_FILL_DIR_PLUS : 0};
    return rc ? rc : fs.ops->list(&fs.remote,path,emit,&l);
}
static int nf_read(const char *p,char *out,size_t count,off_t offset,struct fuse_file_info *info) {
    (void)p;
    struct nf_file *f=(void *)(uintptr_t)info->fh;
    if (offset<0) return -EINVAL;
    if (f->error) return f->error;
    if (f->cache_fd>=0) {
        ssize_t n=pread(f->cache_fd,out,count,offset);
        return n<0 ? -errno : (int)n;
    }
    int rc=flush_file(f);
    if (rc || healthy()) return rc ? rc : -ENOTCONN;
    if ((uint64_t)offset>=f->size) return 0;
    if (count>f->size-(uint64_t)offset) count=(size_t)(f->size-offset);
    if (!f->read_buffer && !(f->read_buffer=allocate_buffer())) return -ENOMEM;
    size_t done=0;
    while (done<count) {
        uint64_t at=(uint64_t)offset+done;
        if (!(at>=f->read_offset && at<f->read_offset+f->read_count)) {
            /* File browsers sniff a few KB; reserve deep readahead for streams. */
            size_t window=(count>=128U*1024 ||
                (f->read_count && at==f->read_offset+f->read_count)) ? NF_WINDOW : 32U*1024;
            size_t n=f->size-at>window ? window : (size_t)(f->size-at);
            rc=fs.ops->read(&fs.remote,f->remote,f->read_buffer,n,at);
            if (rc<=0) return done ? (int)done : rc;
            f->read_offset=at;f->read_count=rc;
            fs.read+=(uint64_t)rc;status_update(0);
        }
        size_t available=(size_t)(f->read_offset+f->read_count-at);
        size_t n=count-done<available ? count-done : available;
        memcpy(out+done,f->read_buffer+(at-f->read_offset),n);
        done+=n;
    }
    return (int)done;
}
static int nf_write(const char *p,const char *input,size_t count,off_t offset,struct fuse_file_info *info) {
    (void)p;
    struct nf_file *f=(void *)(uintptr_t)info->fh;
    if (offset<0 || count>INT_MAX || (uint64_t)offset>INT64_MAX-count) return -EINVAL;
    if (f->error) return f->error;
    if (healthy()) return file_error(f,-ENOTCONN);
    f->read_count=0;
    if (f->cache_fd>=0) {
        struct statvfs free_space;
        if ((uint64_t)offset+count>25ULL*1024*NF_BLOCK || fstatvfs(f->cache_fd,&free_space) ||
            (uint64_t)free_space.f_bavail*free_space.f_frsize<1024ULL*NF_BLOCK+count) return file_error(f,-ENOSPC);
        ssize_t n=pwrite(f->cache_fd,input,count,offset);
        if (n<0) return file_error(f,-errno);
        if ((uint64_t)offset+(uint64_t)n>f->size) f->size=(uint64_t)offset+n;
        f->dirty=1;status_update(0);return (int)n;
    }
    if (!f->write_buffer && !(f->write_buffer=allocate_buffer())) return -ENOMEM;
    if (f->write_count && (uint64_t)offset!=f->write_offset+f->write_count) {
        int rc=drain(f);if (rc) return rc;
    }
    size_t done=0;
    while (done<count) {
        if (!f->write_count) f->write_offset=(uint64_t)offset+done;
        size_t available=NF_WINDOW-f->write_count;
        size_t n=count-done<available ? count-done : available;
        memcpy(f->write_buffer+f->write_count,input+done,n);
        f->write_count+=n;done+=n;f->dirty=1;
        if ((uint64_t)offset+done>f->size) f->size=(uint64_t)offset+done;
        status_update(0);
        if (f->write_count==NF_WINDOW) { int rc=drain(f);if (rc) return rc; }
    }
    return (int)done;
}
static int nf_flush(const char *p,struct fuse_file_info *info) { (void)p;return flush_file((void *)(uintptr_t)info->fh); }
static int nf_fsync(const char *p,int data,struct fuse_file_info *info) { (void)data;return nf_flush(p,info); }
static int nf_release(const char *p,struct fuse_file_info *info) {
    (void)p;struct nf_file *f=(void *)(uintptr_t)info->fh;
    int rc=flush_file(f);
    int closed=healthy() ? -ENOTCONN : fs.ops->close(&fs.remote,f->remote);
    if (!rc && closed && (f->flags&O_ACCMODE)!=O_RDONLY) rc=file_error(f,closed);
    if (f->cache_fd>=0) {
        close(f->cache_fd);
        if (!rc || (rc==-ECANCELED && upload_cancelled(f))) {
            unlink(f->cache_path);unlink(f->manifest);
        }
        else { fs.retained+=f->size;++fs.retained_files; }
    }
    struct nf_file **link=&fs.files;
    while (*link && *link!=f) link=&(*link)->next;
    if (*link) *link=f->next;
    free_buffer(&f->write_buffer);free_buffer(&f->read_buffer);
    free(f->path);free(f->cancel_path);free(f);status_update(1);
    return rc;
}
static int flush_path(const char *path,int descendants) {
    for (struct nf_file *f=fs.files;f;f=f->next) {
        if (strcmp(f->path,path) && (!descendants || strncmp(f->path,path,strlen(path)) || f->path[strlen(path)]!='/')) continue;
        int rc=flush_file(f);if (rc) return rc;
    }
    return healthy();
}
static int nf_truncate(const char *p,off_t size,struct fuse_file_info *info) {
    if (size<0) return -EINVAL;
    char path[8192];int rc=nf_path(fs.config,p,path,sizeof(path));if (rc) return rc;
    rc=flush_path(path,0);if (rc) return rc;
    struct nf_file *f=info ? (void *)(uintptr_t)info->fh : NULL;
    rc=fs.ops->truncate(&fs.remote,path,f ? f->remote : NULL,(uint64_t)size);
    if (!rc) for (f=fs.files;f;f=f->next) if (!strcmp(f->path,path)) {
        if (f->cache_fd>=0 && ftruncate(f->cache_fd,size)) return file_error(f,-errno);
        f->size=(uint64_t)size;f->read_count=0;
    }
    return rc;
}
static int nf_mkdir(const char *p,mode_t mode) {
    if (healthy()) return -ENOTCONN;
    char path[8192];int rc=nf_path(fs.config,p,path,sizeof(path));
    return rc ? rc : fs.ops->mkdir(&fs.remote,path,mode);
}
static int remove_path(const char *p,int dir) {
    char path[8192];int rc=nf_path(fs.config,p,path,sizeof(path));
    if (!rc) rc=flush_path(path,dir);
    return rc ? rc : fs.ops->unlink(&fs.remote,path,dir);
}
static int nf_unlink(const char *p) { return remove_path(p,0); }
static int nf_rmdir(const char *p) { return remove_path(p,1); }
static int nf_rename(const char *a,const char *b,unsigned flags) {
    int rc=healthy();if (rc) return rc;
    char from[8192],to[8192];
    if ((rc=nf_path(fs.config,a,from,sizeof(from))) || (rc=nf_path(fs.config,b,to,sizeof(to)))) return rc;
    /* An open staging handle must not be detached from its recovery manifest. */
    for (struct nf_file *f=fs.files;f;f=f->next)
        if (!strcmp(f->path,from) || (!strncmp(f->path,from,strlen(from)) && f->path[strlen(from)]=='/')) return -EBUSY;
    return fs.ops->rename(&fs.remote,from,to,flags);
}
static int nf_chmod(const char *p,mode_t mode,struct fuse_file_info *info) {
    (void)info;if (healthy()) return -ENOTCONN;
    char path[8192];int rc=nf_path(fs.config,p,path,sizeof(path));return rc ? rc : fs.ops->chmod(&fs.remote,path,mode);
}
static int nf_chown(const char *p,uid_t uid,gid_t gid,struct fuse_file_info *info) {
    (void)info;if (healthy()) return -ENOTCONN;
    char path[8192];int rc=nf_path(fs.config,p,path,sizeof(path));return rc ? rc : fs.ops->chown(&fs.remote,path,uid,gid);
}
static int nf_utimens(const char *p,const struct timespec times[2],struct fuse_file_info *info) {
    (void)info;char path[8192];int rc=nf_path(fs.config,p,path,sizeof(path));
    if (!rc) rc=flush_path(path,0);
    return rc ? rc : fs.ops->utimens(&fs.remote,path,times);
}
static int refresh_space(void) {
    if (healthy()) return -ENOTCONN;
    char root[8192];int rc=nf_path(fs.config,"/",root,sizeof(root));
    struct statvfs st;
    if (!rc) rc=fs.ops->statfs(&fs.remote,root,&st);
    if (!rc) {
        pthread_mutex_lock(&space_mutex);
        fs.space=st;
        pthread_mutex_unlock(&space_mutex);
    }
    return rc;
}
static int nf_statfs(const char *p,struct statvfs *st) {
    (void)p;
    pthread_mutex_lock(&space_mutex);*st=fs.space;pthread_mutex_unlock(&space_mutex);
    return st->f_bsize ? 0 : -ENOTCONN;
}
static int nf_readlink(const char *p,char *buffer,size_t n) {
    if (!n) return -EINVAL;
    if (healthy()) return -ENOTCONN;
    char path[8192];int rc=nf_path(fs.config,p,path,sizeof(path));return rc ? rc : fs.ops->readlink(&fs.remote,path,buffer,n);
}
static int nf_symlink(const char *target,const char *p) {
    if (healthy()) return -ENOTCONN;
    char path[8192];int rc=nf_path(fs.config,p,path,sizeof(path));return rc ? rc : fs.ops->symlink(&fs.remote,target,path);
}
static int nf_listxattr(const char *p,char *b,size_t n) { (void)p;(void)b;(void)n;return 0; }
static void *nf_init(struct fuse_conn_info *conn,struct fuse_config *config) {
    config->entry_timeout=0;config->attr_timeout=1;config->negative_timeout=0;
    config->nullpath_ok=0;
    conn->max_write=NF_BLOCK;conn->max_read=NF_BLOCK;conn->max_readahead=NF_WINDOW;
    /* Userspace owns buffering; kernel writeback must not hide remote errors. */
    conn->want &= ~(FUSE_CAP_WRITEBACK_CACHE|FUSE_CAP_ASYNC_READ);
    conn->want |= conn->capable & FUSE_CAP_READDIRPLUS;
    pthread_mutex_lock(&remote_mutex);status_update(1);pthread_mutex_unlock(&remote_mutex);
    return NULL;
}

#define SERIAL_CALL(name,params,args) \
static int serial_##name params { \
    pthread_mutex_lock(&remote_mutex); \
    int rc=nf_##name args; \
    pthread_mutex_unlock(&remote_mutex);return rc; \
}
SERIAL_CALL(getattr,(const char *p,struct stat *s,struct fuse_file_info *f),(p,s,f))
SERIAL_CALL(readdir,(const char *p,void *b,fuse_fill_dir_t fill,off_t o,struct fuse_file_info *f,enum fuse_readdir_flags flags),(p,b,fill,o,f,flags))
SERIAL_CALL(open,(const char *p,struct fuse_file_info *f),(p,f))
SERIAL_CALL(create,(const char *p,mode_t m,struct fuse_file_info *f),(p,m,f))
SERIAL_CALL(read,(const char *p,char *b,size_t n,off_t o,struct fuse_file_info *f),(p,b,n,o,f))
SERIAL_CALL(write,(const char *p,const char *b,size_t n,off_t o,struct fuse_file_info *f),(p,b,n,o,f))
SERIAL_CALL(flush,(const char *p,struct fuse_file_info *f),(p,f))
SERIAL_CALL(fsync,(const char *p,int d,struct fuse_file_info *f),(p,d,f))
SERIAL_CALL(release,(const char *p,struct fuse_file_info *f),(p,f))
SERIAL_CALL(truncate,(const char *p,off_t n,struct fuse_file_info *f),(p,n,f))
SERIAL_CALL(mkdir,(const char *p,mode_t m),(p,m))
SERIAL_CALL(unlink,(const char *p),(p))
SERIAL_CALL(rmdir,(const char *p),(p))
SERIAL_CALL(rename,(const char *p,const char *q,unsigned flags),(p,q,flags))
SERIAL_CALL(chmod,(const char *p,mode_t m,struct fuse_file_info *f),(p,m,f))
SERIAL_CALL(chown,(const char *p,uid_t u,gid_t g,struct fuse_file_info *f),(p,u,g,f))
SERIAL_CALL(utimens,(const char *p,const struct timespec t[2],struct fuse_file_info *f),(p,t,f))
SERIAL_CALL(readlink,(const char *p,char *b,size_t n),(p,b,n))
SERIAL_CALL(symlink,(const char *t,const char *p),(t,p))
#undef SERIAL_CALL
static int cached_getattr(const char *p,struct stat *st,struct fuse_file_info *info) {
    if (!strcmp(p,"/") && (!info || !info->fh)) { *st=fs.root;return 0; }
    return serial_getattr(p,st,info);
}
static void *maintain_space(void *unused) {
    (void)unused;
    pthread_mutex_lock(&maintenance_mutex);
    while (!maintenance_stop) {
        struct timespec wake;clock_gettime(CLOCK_REALTIME,&wake);wake.tv_sec+=10;
        pthread_cond_timedwait(&maintenance_changed,&maintenance_mutex,&wake);
        if (maintenance_stop) break;
        pthread_mutex_unlock(&maintenance_mutex);
        if (!pthread_mutex_trylock(&remote_mutex)) {
            refresh_space();pthread_mutex_unlock(&remote_mutex);
        }
        pthread_mutex_lock(&maintenance_mutex);
    }
    pthread_mutex_unlock(&maintenance_mutex);
    return NULL;
}
static const struct fuse_operations operations={
    .init=nf_init,.getattr=cached_getattr,.readdir=serial_readdir,.open=serial_open,.create=serial_create,
    .read=serial_read,.write=serial_write,.flush=serial_flush,.fsync=serial_fsync,.release=serial_release,
    .truncate=serial_truncate,.mkdir=serial_mkdir,.unlink=serial_unlink,.rmdir=serial_rmdir,.rename=serial_rename,
    .chmod=serial_chmod,.chown=serial_chown,.utimens=serial_utimens,.statfs=nf_statfs,
    .readlink=serial_readlink,.symlink=serial_symlink,.listxattr=nf_listxattr
};
int nf_mount(struct nf_config *config,const char *mountpoint) {
    memset(&fs,0,sizeof(fs));fs.config=config;fs.remote.socket=-1;
    fs.backend=!strcmp(config->type,"smb") ? &nf_smb_ops : &nf_ssh_ops;fs.ops=&timed_ops;
    if (*config->status_file && snprintf(fs.status_temp,sizeof(fs.status_temp),"%s.%d.tmp",config->status_file,getpid())>=(int)sizeof(fs.status_temp)) return -ENAMETOOLONG;
    int rc=fs.backend->connect(&fs.remote,config,0);
    if (rc) { fs.backend->disconnect(&fs.remote);return rc; }
    rc=nf_deadline_init(&fs.deadline);
    /* The mount root is local metadata; the server still authorizes each real operation. */
    fs.root.st_mode=S_IFDIR|0777;fs.root.st_nlink=2;fs.root.st_ino=1;
    fs.root.st_blksize=NF_BLOCK;fs.root.st_atime=fs.root.st_mtime=fs.root.st_ctime=time(NULL);
    if (!rc) {
        /* Some SFTP servers do not implement the optional statvfs extension. */
        refresh_space();
        if (fs.offline) rc=-ENOTCONN;
    }
    pthread_t maintenance;
    maintenance_stop=0;
    if (!rc) rc=-pthread_create(&maintenance,NULL,maintain_space,NULL);
    if (rc) {
        nf_deadline_destroy(&fs.deadline);fs.backend->disconnect(&fs.remote);return rc;
    }
    const char *arguments[]={"aera-netfs",mountpoint,"-f","-o","fsname=aera-nas,subtype=aera-netfs,allow_other,default_permissions,max_read=1048576,max_threads=8,max_idle_threads=8"};
    rc=fuse_main((int)(sizeof(arguments)/sizeof(arguments[0])),(char **)arguments,&operations,NULL);
    /* Never remove disk-cache files here: unclean exits may need recovery. */
    pthread_mutex_lock(&maintenance_mutex);maintenance_stop=1;pthread_cond_signal(&maintenance_changed);
    pthread_mutex_unlock(&maintenance_mutex);pthread_join(maintenance,NULL);
    /* Closing an offline transport must not wait for a remote goodbye. */
    if (fs.offline) {
        int fd=fs.backend->socket_fd(&fs.remote);
        if (fd>=0) shutdown(fd,SHUT_RDWR);
    }
    int guarded=nf_deadline_begin(&fs.deadline,fs.backend->socket_fd(&fs.remote),5000)==0;
    fs.backend->disconnect(&fs.remote);
    if (guarded) nf_deadline_end(&fs.deadline);
    nf_deadline_destroy(&fs.deadline);
    unlink(config->status_file);
    if (*fs.status_temp) unlink(fs.status_temp);
    return rc ? -EIO : 0;
}
