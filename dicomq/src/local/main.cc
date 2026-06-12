// dicomq-local — maildir-style local delivery (qmail-local analog).
//
//   dicomq-local [-s <spool>] <id> <dir> [env]
//
// Delivers queue/todo/<id> into <dir>/new/. With "env", the envelope is
// copied to <dir>/new/<id>.env FIRST so the object's appearance remains
// the commit point. The object is hardlinked when <dir> is on the spool
// filesystem (EEXIST = already delivered = success); when it is not
// (link gives EXDEV), it is copied through the maildir's own tmp/ and
// committed by rename — which is what maildirs have tmp/ for. Never
// creates <dir> or its subdirectories. Delivered files may share an
// inode with the spool: consumers may move or delete them, never modify
// in place.
//
// Exit: 0 delivered; 100 bad usage; 111 temporary failure (missing
// target, unreadable message) — the caller leaves the message queued.

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

#include "common/message.h"
#include "common/spool.h"

using namespace dicomq;

static bool isDir(const std::string& path)
{
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// link into the maildir, falling back to copy-through-tmp across
// filesystems; idempotent either way
static bool deliverFile(const std::string& src, const std::string& dir,
                        const std::string& name, std::string& err)
{
  const std::string dst = dir + "/new/" + name;
  if (link(src.c_str(), dst.c_str()) == 0 || errno == EEXIST)
    return fsyncPath(dir + "/new", err);
  if (errno != EXDEV)
  {
    err = "cannot link '" + src + "' to '" + dst + "': " + strerror(errno);
    return false;
  }
  const std::string tmp = dir + "/tmp/" + name;
  if (!copyFile(src, tmp, err))
    return false;
  if (!commitFile(tmp, dst, err))
  {
    unlink(tmp.c_str());
    return false;
  }
  return true;
}

int main(int argc, char **argv)
{
  std::string spoolArg;
  int opt;
  while ((opt = getopt(argc, argv, "s:")) != -1)
  {
    if (opt == 's')
      spoolArg = optarg;
    else
    {
      std::fprintf(stderr, "usage: dicomq-local [-s <spool>] <id> <dir> [env]\n");
      return 100;
    }
  }
  if (argc - optind < 2 || argc - optind > 3
      || (argc - optind == 3 && strcmp(argv[optind + 2], "env") != 0))
  {
    std::fprintf(stderr, "usage: dicomq-local [-s <spool>] <id> <dir> [env]\n");
    return 100;
  }
  const std::string id = argv[optind];
  const std::string dir = argv[optind + 1];
  const bool withEnv = (argc - optind == 3);

  const Spool sp(spoolArg);
  std::string err;

  if (!isDir(dir + "/new"))
  {
    std::fprintf(stderr, "dicomq-local: '%s/new' is not a directory "
                 "(dicomq never creates maildirs)\n", dir.c_str());
    return 111;
  }

  // envelope first (when requested): the object's appearance commits
  if (withEnv && !deliverFile(envPath(sp.queueTodo(), id), dir, id + ".env", err))
  {
    std::fprintf(stderr, "dicomq-local: %s\n", err.c_str());
    return 111;
  }
  if (!deliverFile(dcmPath(sp.queueTodo(), id), dir, id + ".dcm", err))
  {
    std::fprintf(stderr, "dicomq-local: %s\n", err.c_str());
    return 111;
  }
  return 0;
}
