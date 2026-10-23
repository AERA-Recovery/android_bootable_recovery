/* SPDX-License-Identifier: Apache-2.0 */
#include "../../nas/native/filesystem.c"
#include <assert.h>

static unsigned writes;
static int cancel_during_write;
static char marker[PATH_MAX];
static uint64_t expected_size;

double nf_now(void) { return 1.0; }
int nf_path(const struct nf_config *config,const char *path,char *out,size_t size) {
    (void)config;
    int n=snprintf(out,size,"%s",path);
    return n<0 || (size_t)n>=size ? -ENAMETOOLONG : 0;
}
void nf_log(const struct nf_config *config, const char *text, int error) {
    (void)config; (void)text; (void)error;
}
static void cancel_upload(const char *scope) {
    int fd=open(marker,O_WRONLY|O_CREAT|O_TRUNC,0600);
    assert(fd>=0);
    assert(write(fd,scope,strlen(scope))==(ssize_t)strlen(scope));
    assert(!close(fd));
}
static int send_bytes(struct nf_remote *remote,void *file,const void *data,size_t count,uint64_t offset) {
    (void)remote; (void)file; (void)data;
    assert(offset==(uint64_t)writes*NF_WINDOW);
    ++writes;
    if (cancel_during_write) cancel_upload("/backups/test\n");
    return (int)count;
}
static int truncate_remote(struct nf_remote *remote,const char *path,void *file,uint64_t size) {
    (void)remote; (void)path; (void)file;
    assert(size==0); return 0;
}
static int confirm_remote(struct nf_remote *remote,void *file) {
    (void)remote; (void)file;
    struct json_object *status=json_object_from_file(fs.config->status_file);
    assert(status);
    struct json_object *value;
    assert(json_object_object_get_ex(status,"uploading_files",&value));
    assert(json_object_get_uint64(value)==1);
    assert(json_object_object_get_ex(status,"uploading_bytes",&value));
    assert(json_object_get_uint64(value)==expected_size);
    assert(json_object_object_get_ex(status,"pending_bytes",&value));
    assert(json_object_get_uint64(value)==0);
    json_object_put(status); return 0;
}
static int stat_remote(struct nf_remote *remote,const char *path,struct stat *st,void *file) {
    (void)remote; (void)path; (void)file;
    memset(st,0,sizeof(*st)); st->st_size=(off_t)expected_size; return 0;
}
static int close_remote(struct nf_remote *remote,void *file) {
    (void)remote; (void)file; return 0;
}
int main(void) {
    char directory[]="/tmp/aera-nas-upload-test-XXXXXX";
    assert(mkdtemp(directory));
    char status[PATH_MAX],cache[PATH_MAX];
    snprintf(status,sizeof(status),"%s/status.json",directory);
    snprintf(marker,sizeof(marker),"%s/status.json.cancel",directory);
    snprintf(cache,sizeof(cache),"%s/cache",directory);
    snprintf(fs.status_temp,sizeof(fs.status_temp),"%s/status.tmp",directory);
    struct nf_config config={.type="smb",.cache_dir=directory,.status_file=status};
    const struct nf_ops ops={.write=send_bytes,.flush=confirm_remote,.truncate=truncate_remote,.stat=stat_remote,.close=close_remote};
    fs.config=&config;fs.ops=&ops;fs.remote.smb=(void *)1;
    struct nf_file file={.path="server/base/backups/test/boot.win",.cancel_path="/backups/test/boot.win",.dirty=1};
    file.cache_fd=open(cache,O_RDWR|O_CREAT|O_EXCL,0600);
    assert(file.cache_fd>=0);
    file.size=expected_size=2ULL*NF_WINDOW+257;
    assert(!ftruncate(file.cache_fd,(off_t)file.size));
    fs.files=&file;
    cancel_upload("/backups/test\n");
    assert(flush_file(&file)==-ECANCELED && writes==0 && file.dirty);
    unlink(marker);file.error=0;cancel_during_write=1;
    assert(flush_file(&file)==-ECANCELED && writes==1 && file.dirty && !file.uploading);
    assert(file.uploaded==NF_WINDOW);
    cancel_upload("/backups/tes\n");
    file.error=0;cancel_during_write=0;writes=0;
    assert(!flush_file(&file));
    assert(writes==3 && !file.dirty && !file.uploading && !fs.memory);
    assert(!close(file.cache_fd));
    struct nf_file *cancelled=calloc(1,sizeof(*cancelled));
    assert(cancelled);
    cancelled->path=strdup(file.path); cancelled->cancel_path=strdup(file.cancel_path);
    cancelled->cache_fd=open(cache,O_RDWR); assert(cancelled->cache_fd>=0);
    cancelled->dirty=1; cancelled->error=-ECANCELED;
    snprintf(cancelled->cache_path,sizeof(cancelled->cache_path),"%s",cache);
    snprintf(cancelled->manifest,sizeof(cancelled->manifest),"%s/manifest",directory);
    int manifest=open(cancelled->manifest,O_WRONLY|O_CREAT|O_EXCL,0600);
    assert(manifest>=0 && !close(manifest));
    fs.files=cancelled;
    struct fuse_file_info info={.fh=(uint64_t)(uintptr_t)cancelled};
    cancel_upload("/backups/test\n");
    assert(nf_release("/backups/test/boot.win",&info)==-ECANCELED);
    assert(!fs.files && !fs.retained_files && access(cache,F_OK));
    /* Failed network flushes retain both data and its recovery manifest. */
    struct nf_file *offline=calloc(1,sizeof(*offline));assert(offline);
    offline->path=strdup(file.path);offline->cancel_path=strdup(file.cancel_path);
    offline->cache_fd=open(cache,O_RDWR|O_CREAT|O_EXCL,0600);assert(offline->cache_fd>=0);
    offline->dirty=1;offline->size=321;
    assert(!ftruncate(offline->cache_fd,321));
    snprintf(offline->cache_path,sizeof(offline->cache_path),"%s",cache);
    snprintf(offline->manifest,sizeof(offline->manifest),"%s/manifest",directory);
    manifest=open(offline->manifest,O_WRONLY|O_CREAT|O_EXCL,0600);assert(manifest>=0 && !close(manifest));
    fs.files=offline;fs.offline=1;info.fh=(uint64_t)(uintptr_t)offline;
    unlink(marker);
    assert(nf_write("/backups/test/boot.win","x",1,321,&info)==-ENOTCONN);
    assert(nf_release("/backups/test/boot.win",&info)==-ENOTCONN);
    assert(!fs.files && fs.retained_files==1 && fs.retained==321);
    assert(!access(cache,F_OK));
    char manifest_path[PATH_MAX];snprintf(manifest_path,sizeof(manifest_path),"%s/manifest",directory);
    assert(!access(manifest_path,F_OK));unlink(manifest_path);
    struct json_object *lost=json_object_from_file(status);assert(lost);
    assert(!json_object_get_boolean(json_object_object_get(lost,"connected")));
    json_object_put(lost);
    /* Mount metadata must remain available while a remote request owns the lock. */
    fs.root.st_mode=S_IFDIR|0777;fs.space.f_bsize=4096;
    pthread_mutex_lock(&remote_mutex);
    struct stat root;struct statvfs space;
    assert(!cached_getattr("/",&root,NULL) && S_ISDIR(root.st_mode));
    assert(!nf_statfs("/",&space) && space.f_bsize==4096);
    pthread_mutex_unlock(&remote_mutex);
    unlink(marker);unlink(cache);unlink(status);unlink(fs.status_temp);rmdir(directory);
    puts("NAS upload: real byte progress, final confirmation and scoped cancellation passed.");
    return 0;
}
