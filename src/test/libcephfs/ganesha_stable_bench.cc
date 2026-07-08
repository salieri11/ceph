// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab
//
// Stable write benchmark using libcephfs ll_* APIs, matching nfs-ganesha
// FSAL_CEPH ceph_fsal_write2() when fsal_stable is set:
//   ceph_ll_write() [+ ceph_ll_fsync()]  (sync path)
//   ceph_ll_nonblocking_readv_writev()    (async path)

#include "include/cephfs/libcephfs.h"
#include "include/compat.h"

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <getopt.h>
#include <semaphore.h>
#include <string>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

namespace {

struct Config {
  std::string conf_path;
  std::string client_id = "admin";
  std::string fs_name;
  std::string root_path;
  std::string filename = "ganesha_stable_bench.dat";
  std::string result_file;
  uint64_t bs = 4096;
  uint64_t filesize = 64 * 1024 * 1024;
  int duration = 120;
  int stable = 1;
  int async = 0;
  int syncdataonly = 0;
  int verbose = 0;
};

struct AsyncWait {
  sem_t sem;
  int done = 0;
  int64_t result = 0;

  AsyncWait() { sem_init(&sem, 0, 0); }
  ~AsyncWait() { sem_destroy(&sem); }
};

static void ll_io_callback(struct ceph_ll_io_info *info)
{
  auto *wait = static_cast<AsyncWait *>(info->priv);
  wait->result = info->result;
  wait->done = 1;
  sem_post(&wait->sem);
}

static void usage(const char *prog)
{
  fprintf(stderr,
	  "Usage: %s [OPTIONS]\n"
	  "\n"
	  "Simulate Ganesha FSAL_CEPH stable writes via libcephfs ll_* APIs.\n"
	  "\n"
	  "  --conf PATH         ceph.conf (required unless CEPH_CONF is set)\n"
	  "  --id ID             client id without 'client.' prefix (default: admin)\n"
	  "  --fs-name NAME      CephFS filesystem name (required)\n"
	  "  --root PATH         Subvolume root path for ceph_mount (required)\n"
	  "  --file NAME         Benchmark file name (default: ganesha_stable_bench.dat)\n"
	  "  --bs SIZE           Write size, e.g. 4k, 32k (default: 4k)\n"
	  "  --filesize SIZE     File size before wrap, e.g. 64m (default: 64m)\n"
	  "  --duration SECS     Run duration (default: 120)\n"
	  "  --stable 0|1        fsal_stable: fsync after each write (default: 1)\n"
	  "  --async 0|1         Use ceph_ll_nonblocking_readv_writev (default: 0)\n"
	  "  --syncdataonly 0|1  ll_fsync syncdataonly flag (Ganesha uses 0, default: 0)\n"
	  "  --result-file PATH  Write stable-write op count to file\n"
	  "  --verbose           Progress logging\n"
	  "  -h, --help          Show this help\n",
	  prog);
}

static int parse_size(const char *s, uint64_t *out)
{
  char *end = nullptr;
  errno = 0;
  unsigned long long v = strtoull(s, &end, 0);
  if (errno || end == s) {
    return -EINVAL;
  }

  uint64_t mul = 1;
  if (*end) {
    switch (*end) {
    case 'k':
    case 'K':
      mul = 1024;
      ++end;
      break;
    case 'm':
    case 'M':
      mul = 1024 * 1024;
      ++end;
      break;
    case 'g':
    case 'G':
      mul = 1024ULL * 1024 * 1024;
      ++end;
      break;
    default:
      return -EINVAL;
    }
    if (*end != '\0') {
      return -EINVAL;
    }
  }

  *out = v * mul;
  return 0;
}

static int parse_nonneg_int(const char *s, int *out)
{
  char *end = nullptr;
  errno = 0;
  long v = strtol(s, &end, 10);
  if (errno || end == s || *end != '\0' || v < 0) {
    return -EINVAL;
  }
  *out = static_cast<int>(v);
  return 0;
}

static int parse_config(int argc, char **argv, Config *cfg, const char *prog)
{
  static const struct option opts[] = {
    {"conf", required_argument, nullptr, 'c'},
    {"id", required_argument, nullptr, 'i'},
    {"fs-name", required_argument, nullptr, 'f'},
    {"root", required_argument, nullptr, 'r'},
    {"file", required_argument, nullptr, 'F'},
    {"bs", required_argument, nullptr, 'b'},
    {"filesize", required_argument, nullptr, 's'},
    {"duration", required_argument, nullptr, 'd'},
    {"stable", required_argument, nullptr, 'S'},
    {"async", required_argument, nullptr, 'a'},
    {"syncdataonly", required_argument, nullptr, 'D'},
    {"result-file", required_argument, nullptr, 'o'},
    {"verbose", no_argument, nullptr, 'v'},
    {"help", no_argument, nullptr, 'h'},
    {nullptr, 0, nullptr, 0},
  };

  int c;
  while ((c = getopt_long(argc, argv, "c:i:f:r:F:b:s:d:S:a:D:o:vh", opts, nullptr)) != -1) {
    switch (c) {
    case 'c':
      cfg->conf_path = optarg;
      break;
    case 'i':
      cfg->client_id = optarg;
      break;
    case 'f':
      cfg->fs_name = optarg;
      break;
    case 'r':
      cfg->root_path = optarg;
      break;
    case 'F':
      cfg->filename = optarg;
      break;
    case 'b':
      if (parse_size(optarg, &cfg->bs) < 0) {
	return -EINVAL;
      }
      break;
    case 's':
      if (parse_size(optarg, &cfg->filesize) < 0) {
	return -EINVAL;
      }
      break;
    case 'd':
      if (parse_nonneg_int(optarg, &cfg->duration) < 0) {
	return -EINVAL;
      }
      break;
    case 'S':
      if (parse_nonneg_int(optarg, &cfg->stable) < 0 || cfg->stable > 1) {
	return -EINVAL;
      }
      break;
    case 'a':
      if (parse_nonneg_int(optarg, &cfg->async) < 0 || cfg->async > 1) {
	return -EINVAL;
      }
      break;
    case 'D':
      if (parse_nonneg_int(optarg, &cfg->syncdataonly) < 0 || cfg->syncdataonly > 1) {
	return -EINVAL;
      }
      break;
    case 'o':
      cfg->result_file = optarg;
      break;
    case 'v':
      cfg->verbose = 1;
      break;
    case 'h':
      usage(prog);
      exit(0);
    default:
      return -EINVAL;
    }
  }

  if (cfg->fs_name.empty() || cfg->root_path.empty()) {
    fprintf(stderr, "ERROR: --fs-name and --root are required\n");
    usage(prog);
    return -EINVAL;
  }
  if (cfg->bs == 0 || cfg->filesize == 0) {
    fprintf(stderr, "ERROR: --bs and --filesize must be non-zero\n");
    return -EINVAL;
  }
  if (cfg->filesize < cfg->bs) {
    cfg->filesize = cfg->bs;
  }
  return 0;
}

static double now_mono()
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int mount_client(const Config &cfg, ceph_mount_info **cmount_out)
{
  ceph_mount_info *cmount = nullptr;
  int ret = ceph_create(&cmount, cfg.client_id.c_str());
  if (ret < 0) {
    fprintf(stderr, "ERROR: ceph_create failed: %s\n", strerror(-ret));
    return ret;
  }

  const char *conf = cfg.conf_path.empty() ? nullptr : cfg.conf_path.c_str();
  ret = ceph_conf_read_file(cmount, conf);
  if (ret < 0) {
    fprintf(stderr, "ERROR: ceph_conf_read_file failed: %s\n", strerror(-ret));
    ceph_release(cmount);
    return ret;
  }

  ret = ceph_conf_parse_env(cmount, nullptr);
  if (ret < 0) {
    fprintf(stderr, "ERROR: ceph_conf_parse_env failed: %s\n", strerror(-ret));
    ceph_release(cmount);
    return ret;
  }

  ret = ceph_select_filesystem(cmount, cfg.fs_name.c_str());
  if (ret < 0) {
    fprintf(stderr, "ERROR: ceph_select_filesystem(%s) failed: %s\n",
	    cfg.fs_name.c_str(), strerror(-ret));
    ceph_release(cmount);
    return ret;
  }

  ret = ceph_mount(cmount, cfg.root_path.c_str());
  if (ret < 0) {
    fprintf(stderr, "ERROR: ceph_mount(%s) failed: %s\n",
	    cfg.root_path.c_str(), strerror(-ret));
    ceph_release(cmount);
    return ret;
  }

  *cmount_out = cmount;
  return 0;
}

static int open_bench_file(ceph_mount_info *cmount, const Config &cfg,
			   Inode **inode_out, Fh **fh_out)
{
  Inode *root = nullptr;
  int ret = ceph_ll_lookup_root(cmount, &root);
  if (ret < 0) {
    fprintf(stderr, "ERROR: ceph_ll_lookup_root failed: %s\n", strerror(-ret));
    return ret;
  }

  UserPerm *perms = ceph_mount_perms(cmount);
  Inode *inode = nullptr;
  Fh *fh = nullptr;
  struct ceph_statx stx {};

  ret = ceph_ll_lookup(cmount, root, cfg.filename.c_str(), &inode, &stx,
		       CEPH_STATX_INO, 0, perms);
  if (ret == -ENOENT) {
    ret = ceph_ll_create(cmount, root, cfg.filename.c_str(), 0644,
			 O_RDWR | O_CREAT | O_TRUNC, &inode, &fh, &stx,
			 CEPH_STATX_INO, 0, perms);
    if (ret < 0) {
      fprintf(stderr, "ERROR: ceph_ll_create(%s) failed: %s\n",
	      cfg.filename.c_str(), strerror(-ret));
      return ret;
    }
  } else if (ret < 0) {
    fprintf(stderr, "ERROR: ceph_ll_lookup(%s) failed: %s\n",
	    cfg.filename.c_str(), strerror(-ret));
    return ret;
  } else {
    ret = ceph_ll_open(cmount, inode, O_RDWR, &fh, perms);
    if (ret < 0) {
      fprintf(stderr, "ERROR: ceph_ll_open failed: %s\n", strerror(-ret));
      return ret;
    }
  }

  *inode_out = inode;
  *fh_out = fh;
  return 0;
}

static int stable_write_sync(ceph_mount_info *cmount, Fh *fh,
			     const Config &cfg, int64_t offset,
			     const char *buf)
{
  int nb = ceph_ll_write(cmount, fh, offset, cfg.bs, buf);
  if (nb < 0) {
    return nb;
  }
  if (static_cast<uint64_t>(nb) != cfg.bs) {
    return -EIO;
  }

  if (!cfg.stable) {
    return 0;
  }

  return ceph_ll_fsync(cmount, fh, cfg.syncdataonly);
}

static int stable_write_async(ceph_mount_info *cmount, Fh *fh,
			      const Config &cfg, int64_t offset, char *buf)
{
  struct iovec iov {};
  iov.iov_base = buf;
  iov.iov_len = cfg.bs;

  AsyncWait wait;
  struct ceph_ll_io_info io_info {};
  io_info.callback = ll_io_callback;
  io_info.priv = &wait;
  io_info.fh = fh;
  io_info.iov = &iov;
  io_info.iovcnt = 1;
  io_info.off = offset;
  io_info.write = true;
  io_info.fsync = cfg.stable ? true : false;
  io_info.syncdataonly = cfg.syncdataonly ? true : false;

  int64_t ret = ceph_ll_nonblocking_readv_writev(cmount, &io_info);
  if (ret < 0) {
    return static_cast<int>(ret);
  }
  if (ret == 0) {
    while (sem_wait(&wait.sem) < 0 && errno == EINTR) {
    }
    if (wait.result < 0) {
      return static_cast<int>(wait.result);
    }
    if (static_cast<uint64_t>(wait.result) != cfg.bs) {
      return -EIO;
    }
    return 0;
  }

  if (static_cast<uint64_t>(ret) != cfg.bs) {
    return -EIO;
  }
  return 0;
}

static int write_result_file(const std::string &path, uint64_t ops)
{
  if (path.empty()) {
    return 0;
  }
  FILE *f = fopen(path.c_str(), "w");
  if (!f) {
    fprintf(stderr, "ERROR: cannot write result file %s: %s\n",
	    path.c_str(), strerror(errno));
    return -errno;
  }
  fprintf(f, "%" PRIu64 "\n", ops);
  fclose(f);
  return 0;
}

} // namespace

int main(int argc, char **argv)
{
  Config cfg;
  if (parse_config(argc, argv, &cfg, argv[0]) < 0) {
    usage(argv[0]);
    return 2;
  }

  ceph_mount_info *cmount = nullptr;
  if (mount_client(cfg, &cmount) < 0) {
    return 1;
  }

  Inode *inode = nullptr;
  Fh *fh = nullptr;
  if (open_bench_file(cmount, cfg, &inode, &fh) < 0) {
    ceph_unmount(cmount);
    ceph_release(cmount);
    return 1;
  }

  std::string buf(cfg.bs, 'x');
  const double start = now_mono();
  const double deadline = start + cfg.duration;
  uint64_t stable_ops = 0;
  int64_t offset = 0;
  int progress_ops = 0;

  if (cfg.verbose) {
    fprintf(stderr,
	    "[ganesha_stable_bench] root=%s file=%s bs=%" PRIu64
	    " filesize=%" PRIu64 " stable=%d async=%d syncdataonly=%d duration=%ds\n",
	    cfg.root_path.c_str(), cfg.filename.c_str(), cfg.bs, cfg.filesize,
	    cfg.stable, cfg.async, cfg.syncdataonly, cfg.duration);
  }

  while (now_mono() < deadline) {
    int ret;
    if (cfg.async) {
      ret = stable_write_async(cmount, fh, cfg, offset, buf.data());
    } else {
      ret = stable_write_sync(cmount, fh, cfg, offset, buf.data());
    }
    if (ret < 0) {
      fprintf(stderr, "ERROR: stable write at offset %" PRId64 " failed: %s\n",
	      offset, strerror(-ret));
      break;
    }

    ++stable_ops;
    ++progress_ops;
    offset += cfg.bs;
    if (static_cast<uint64_t>(offset) >= cfg.filesize) {
      offset = 0;
    }

    if (cfg.verbose && progress_ops >= 1000) {
      fprintf(stderr, "[ganesha_stable_bench] %" PRIu64 " stable writes (%.0fs)\n",
	      stable_ops, now_mono() - start);
      progress_ops = 0;
    }
  }

  ceph_ll_close(cmount, fh);
  ceph_unmount(cmount);
  ceph_release(cmount);

  const double elapsed = now_mono() - start;
  const uint64_t rate = static_cast<uint64_t>(
    stable_ops / (elapsed > 0.0 ? elapsed : 1.0));

  printf("stable_writes=%" PRIu64 "\n", stable_ops);
  printf("elapsed=%.3f\n", elapsed);
  printf("stable_writes_per_sec=%" PRIu64 "\n", rate);

  if (write_result_file(cfg.result_file, stable_ops) < 0) {
    return 1;
  }

  if (cfg.verbose) {
    fprintf(stderr,
	    "[ganesha_stable_bench] finished: %" PRIu64 " stable writes in %.1fs (%" PRIu64 "/s)\n",
	    stable_ops, elapsed, rate);
  }

  return 0;
}
