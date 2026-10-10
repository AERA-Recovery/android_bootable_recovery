// Copyright 2026 AERA Recovery Project contributors.
// SPDX-License-Identifier: Apache-2.0
// Direct write phase only. AERA owns target selection, unmount/resizing, exclusive
// claim and mandatory flushed readback against the reviewed manifest afterward.
use super::Cmd;
use crate::payload::{Payload, PayloadData};
use crate::proto::chromeos_update_engine::{DeltaArchiveManifest, PartitionUpdate};
use anyhow::{bail, ensure, Context, Result};
use indicatif::ProgressBar;
use rayon::prelude::*;
use ring::digest::{digest, SHA256};
use std::fs::File;
use std::io::Read;
use std::os::fd::{AsRawFd, BorrowedFd};
use std::os::unix::fs::{FileExt, FileTypeExt};
use std::sync::atomic::{AtomicBool, Ordering};

const LIMIT: u64 = 64 * 1024 * 1024;

fn validate(p: &PartitionUpdate, block: u64, data: &[u8]) -> Result<u64> {
    ensure!(block > 0 && block <= 1024 * 1024 && block.is_power_of_two(), "Invalid block size");
    ensure!(p.old_partition_info.is_none(), "Incremental image is not supported");
    let info = p.new_partition_info.as_ref().context("Missing image information")?;
    let size = info.size.context("Missing image size")?;
    ensure!(size > 0 && size <= i64::MAX as u64 && size % block == 0, "Invalid image size");
    ensure!(info.hash.as_ref().is_some_and(|h| h.len() == 32), "Missing image SHA-256");
    let mut ranges = Vec::new();
    for op in &p.operations {
        ensure!(matches!(op.r#type, 0 | 1 | 6 | 7 | 8) && op.src_extents.is_empty() &&
            op.src_sha256_hash.is_none(), "Unsupported operation");
        let mut decoded = 0u64;
        for extent in &op.dst_extents {
            let start = extent.start_block.context("Missing extent offset")?;
            let count = extent.num_blocks.context("Missing extent length")?;
            ensure!(count > 0 && start < size / block && count <= size / block - start, "Extent outside image");
            decoded = decoded.checked_add(count * block).context("Extent overflow")?;
            ranges.push((start * block, (start + count) * block));
            ensure!(ranges.len() <= 1_000_000, "Too many extents");
        }
        ensure!(decoded > 0, "Empty operation");
        let length = op.data_length.unwrap_or(0);
        if matches!(op.r#type, 6 | 7) {
            ensure!(length == 0, "Unexpected zero-operation data");
        } else {
            let offset = op.data_offset.context("Missing data offset")?;
            ensure!(decoded <= LIMIT && length > 0 && length <= LIMIT, "Operation exceeds memory limit");
            ensure!(offset <= data.len() as u64 && length <= data.len() as u64 - offset, "Truncated operation");
            ensure!(op.data_sha256_hash.as_ref().is_some_and(|h| h.len() == 32), "Missing operation hash");
        }
    }
    ranges.sort_unstable();
    let mut end = 0;
    for (start, next) in ranges {
        ensure!(start == end, "Overlapping or incomplete extents");
        end = next;
    }
    ensure!(end == size, "Incomplete image");
    Ok(size)
}

// Regular files are accepted only by this private worker to allow non-destructive
// unit tests. The command entry below rejects everything except block devices.
fn write_image(file: &File, p: &PartitionUpdate, block: u64, data: &[u8], workers: usize) -> Result<()> {
    let size = validate(p, block, data)?;
    let pb = ProgressBar::hidden();
    let _reporter = super::aera::Reporter::start(vec![pb.clone()], size);
    let cancelled = AtomicBool::new(false);
    let pool = rayon::ThreadPoolBuilder::new().num_threads(workers.clamp(1, 4)).build()?;
    pool.install(|| p.operations.par_iter().try_for_each(|op| -> Result<()> {
        if cancelled.load(Ordering::Acquire) { bail!("Direct flash cancelled after write error"); }
        let result = (|| -> Result<()> {
            let decoded_size = op.dst_extents.iter().map(|e| e.num_blocks.unwrap() * block).sum::<u64>();
            let zero = matches!(op.r#type, 6 | 7);
            let decoded = if zero {
                vec![0; 1024 * 1024]
            } else {
                let offset = op.data_offset.unwrap() as usize;
                let length = op.data_length.unwrap() as usize;
                let input = &data[offset..offset + length];
                ensure!(digest(&SHA256, input).as_ref() == op.data_sha256_hash.as_ref().unwrap(), "Operation SHA-256 mismatch");
                let mut output = Vec::with_capacity(decoded_size as usize);
                match op.r#type {
                    0 => output.extend_from_slice(input),
                    1 => { bzip2::read::BzDecoder::new(input).take(decoded_size + 1).read_to_end(&mut output)?; }
                    8 => {
                        let stream = liblzma::stream::Stream::new_stream_decoder(80 * 1024 * 1024, 0)?;
                        liblzma::read::XzDecoder::new_stream(input, stream).take(decoded_size + 1).read_to_end(&mut output)?;
                    }
                    _ => unreachable!(),
                }
                ensure!(output.len() as u64 == decoded_size, "Decoded operation size mismatch");
                output
            };
            let mut consumed = 0usize;
            for extent in &op.dst_extents {
                let mut offset = extent.start_block.unwrap() * block;
                let mut left = extent.num_blocks.unwrap() * block;
                while left > 0 {
                    ensure!(!cancelled.load(Ordering::Acquire), "Direct flash cancelled");
                    let count = left.min(1024 * 1024) as usize;
                    let bytes = if zero { &decoded[..count] } else { &decoded[consumed..consumed + count] };
                    file.write_all_at(bytes, offset).context("Partition write failed")?;
                    offset += count as u64;
                    left -= count as u64;
                    consumed += count;
                    pb.inc(count as u64);
                }
            }
            Ok(())
        })();
        if result.is_err() { cancelled.store(true, Ordering::Release); }
        result
    }))?;
    file.sync_all().context("Partition flush failed")?;
    Ok(())
}

pub(super) fn run(cmd: &Cmd, payload: &Payload<'_>, manifest: &DeltaArchiveManifest, block: usize) -> Result<()> {
    ensure!(cmd.aera && cmd.strict && !cmd.no_verify && cmd.partitions.len() == 1, "Invalid direct request");
    let expected = hex::decode(cmd.aera_manifest.as_ref().context("Missing reviewed manifest")?)?;
    ensure!(expected.len() == 32 && digest(&SHA256, payload.manifest).as_ref() == expected, "Manifest changed since review");
    let data = match &payload.data {
        PayloadData::Local(bytes) => *bytes,
        #[cfg(feature = "remote")]
        _ => bail!("Direct mode requires a local package"),
    };
    let p = manifest.partitions.iter().find(|p| p.partition_name == cmd.partitions[0]).context("Missing selected partition")?;
    let size = validate(p, block as u64, data)?;
    let fd = cmd.aera_block_fd.context("Missing block descriptor")?;
    ensure!(fd >= 3, "Invalid block descriptor");
    ensure!(unsafe { libc::fcntl(fd, libc::F_GETFD) } >= 0, "Closed block descriptor");
    // Clone the inherited descriptor; never reopen a user-supplied target path.
    let file = File::from(unsafe { BorrowedFd::borrow_raw(fd) }.try_clone_to_owned()?);
    ensure!(file.metadata()?.file_type().is_block_device(), "Direct target is not a block device");
    let mut capacity = 0u64;
    let mut readonly = 1i32;
    ensure!(unsafe { libc::ioctl(file.as_raw_fd(), 0x80081272u32 as _, &mut capacity) } == 0 &&
        unsafe { libc::ioctl(file.as_raw_fd(), 0x125e as _, &mut readonly) } == 0 &&
        readonly == 0 && capacity >= size, "Target is read-only or too small");
    write_image(&file, p, block as u64, data, cmd.threads.unwrap_or(1))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::proto::chromeos_update_engine::{Extent, InstallOperation, PartitionInfo};
    use std::io::Write;

    fn operation(kind: i32, start: u64, blocks: u64, input: &[u8]) -> InstallOperation {
        InstallOperation {
            r#type: kind,
            dst_extents: vec![Extent { start_block: Some(start), num_blocks: Some(blocks) }],
            data_offset: Some(0), data_length: Some(input.len() as u64),
            data_sha256_hash: Some(digest(&SHA256, input).as_ref().to_vec()),
            ..Default::default()
        }
    }
    fn partition(ops: Vec<InstallOperation>, size: u64) -> PartitionUpdate {
        PartitionUpdate { partition_name: "system".into(), operations: ops,
            new_partition_info: Some(PartitionInfo { size: Some(size), hash: Some(vec![0; 32]) }),
            ..Default::default() }
    }
    fn contents(file: &File, size: usize) -> Vec<u8> {
        let mut bytes = vec![0; size];
        file.read_exact_at(&mut bytes, 0).unwrap(); bytes
    }
    #[test]
    fn zeros_replace_old_contents_and_larger_target_tail_survives() {
        let file = tempfile::tempfile().unwrap();
        file.write_all_at(&vec![0x55; 2048], 0).unwrap();
        let data = vec![0xab; 512];
        let p = partition(vec![operation(0, 0, 1, &data), operation(6, 1, 1, &[]), operation(7, 2, 1, &[])], 1536);
        write_image(&file, &p, 512, &data, 4).unwrap();
        let result = contents(&file, 2048);
        assert_eq!(&result[..512], &data);
        assert_eq!(&result[512..1536], &vec![0; 1024]);
        assert_eq!(&result[1536..], &vec![0x55; 512]);
    }
    #[test]
    fn codecs_write_multiple_extents_in_manifest_order() {
        let plain: Vec<u8> = (0..1024).map(|n| (n / 512 + 1) as u8).collect();
        for kind in [0, 1, 8] {
            let input = match kind {
                1 => {
                    let mut e = bzip2::write::BzEncoder::new(Vec::new(), bzip2::Compression::best());
                    e.write_all(&plain).unwrap(); e.finish().unwrap()
                }
                8 => {
                    let mut e = liblzma::write::XzEncoder::new(Vec::new(), 6);
                    e.write_all(&plain).unwrap(); e.finish().unwrap()
                }
                _ => plain.clone(),
            };
            let mut op = operation(kind, 1, 1, &input);
            op.dst_extents.push(Extent { start_block: Some(0), num_blocks: Some(1) });
            let p = partition(vec![op], 1024);
            let file = tempfile::tempfile().unwrap();
            file.set_len(1024).unwrap();
            write_image(&file, &p, 512, &input, 2).unwrap();
            assert_eq!(contents(&file, 1024), [vec![2; 512], vec![1; 512]].concat());
        }
    }
    #[test]
    fn invalid_coverage_and_source_bounds_fail_before_any_write() {
        let data = vec![7; 512];
        for bad in [operation(0, 0, 1, &data), operation(0, 2, 1, &data), operation(4, 1, 1, &data)] {
            let file = tempfile::tempfile().unwrap();
            file.write_all_at(&vec![0x55; 1024], 0).unwrap();
            let p = partition(vec![operation(0, 0, 1, &data), bad], 1024);
            assert!(write_image(&file, &p, 512, &data, 4).is_err());
            assert_eq!(contents(&file, 1024), vec![0x55; 1024]);
        }
        let p = partition(vec![operation(0, 0, 1, &data)], 512);
        assert!(validate(&p, 512, &data[..511]).is_err());
    }
    #[test]
    fn bad_operation_hash_or_decode_length_does_not_write_operation() {
        for mismatch in [true, false] {
            let file = tempfile::tempfile().unwrap();
            file.write_all_at(&vec![0x55; 512], 0).unwrap();
            let data = vec![7; if mismatch { 512 } else { 511 }];
            let mut op = operation(0, 0, 1, &data);
            if mismatch { op.data_sha256_hash = Some(vec![0; 32]); }
            let p = partition(vec![op], 512);
            assert!(write_image(&file, &p, 512, &data, 1).is_err());
            assert_eq!(contents(&file, 512), vec![0x55; 512]);
        }
    }
    #[test]
    fn command_rejects_regular_file_even_with_valid_manifest() {
        use clap::Parser;
        use prost::Message;
        let file = tempfile::tempfile().unwrap();
        file.write_all_at(&vec![0x55; 512], 0).unwrap();
        let data = vec![7; 512];
        let p = partition(vec![operation(0, 0, 1, &data)], 512);
        let manifest = DeltaArchiveManifest { partitions: vec![p], block_size: Some(512), ..Default::default() };
        let bytes = manifest.encode_to_vec();
        let payload = Payload { file_format_version: 2, manifest_size: bytes.len() as u64,
            manifest: &bytes, metadata_signature: None, data: PayloadData::Local(&data) };
        let hash = hex::encode(digest(&SHA256, &bytes));
        let fd = file.as_raw_fd().to_string();
        let cmd = Cmd::try_parse_from(["otaripper", "--aera", "--strict", "--no-open", "--output-dir", "/tmp",
            "--partitions", "system", "--aera-block-fd", &fd, "--aera-manifest", &hash, "input.zip"]).unwrap();
        assert!(run(&cmd, &payload, &manifest, 512).unwrap_err().to_string().contains("not a block"));
        assert_eq!(contents(&file, 512), vec![0x55; 512]);
    }
    #[test]
    #[ignore = "Requires AERA_TEST_FULL_OTA; writes only a temporary regular file"]
    fn real_full_ota_direct_writer() {
        use prost::Message;
        let path = std::env::var("AERA_TEST_FULL_OTA").unwrap();
        let source = File::open(path).unwrap();
        let mapped = unsafe { memmap2::Mmap::map(&source) }.unwrap();
        let mut zip = zip::ZipArchive::new(&source).unwrap();
        let entry = zip.by_name("payload.bin").unwrap();
        assert_eq!(entry.compression(), zip::CompressionMethod::Stored);
        let start = entry.data_start().unwrap() as usize;
        let payload = Payload::parse(&mapped[start..start + entry.size() as usize]).unwrap();
        let manifest = DeltaArchiveManifest::decode(payload.manifest).unwrap();
        let p = manifest.partitions.iter().find(|p| p.partition_name == "system").unwrap();
        let PayloadData::Local(data) = payload.data else { panic!("Not local"); };
        let size = p.new_partition_info.as_ref().unwrap().size.unwrap();
        let output = tempfile::tempfile().unwrap();
        output.set_len(size + 512).unwrap();
        output.write_all_at(&vec![0x55; 512], size).unwrap();
        let start = std::time::Instant::now();
        write_image(&output, p, manifest.block_size.unwrap() as u64, data, 4).unwrap();
        let result = unsafe { memmap2::Mmap::map(&output) }.unwrap();
        assert_eq!(digest(&SHA256, &result[..size as usize]).as_ref(), p.new_partition_info.as_ref().unwrap().hash.as_ref().unwrap());
        assert_eq!(&result[size as usize..], &vec![0x55; 512]);
        eprintln!("Direct writer + SHA-256: {} bytes in {:.3}s", size, start.elapsed().as_secs_f64());
    }
}
