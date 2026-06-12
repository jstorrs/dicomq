#include "common/message.h"

#include <cerrno>
#include <cstring>

#include <sys/stat.h>
#include <unistd.h>

namespace dicomq {

std::string dcmPath(const std::string& dir, const std::string& id)
{
  return dir + "/" + id + ".dcm";
}

std::string envPath(const std::string& dir, const std::string& id)
{
  return dir + "/" + id + ".env";
}

static bool unlinkTolerant(const std::string& path, std::string& err)
{
  if (unlink(path.c_str()) != 0 && errno != ENOENT)
  {
    err = "cannot unlink '" + path + "': " + strerror(errno);
    return false;
  }
  return true;
}

bool removePair(const std::string& dir, const std::string& id,
                std::string& err)
{
  if (!unlinkTolerant(envPath(dir, id), err))
    return false;
  return unlinkTolerant(dcmPath(dir, id), err);
}

bool writeEnvelopeCommitted(const Spool& sp, const Envelope& env,
                            const std::string& finalPath, std::string& err)
{
  // tmp name carries pid so concurrent writers (send + remote agents)
  // cannot collide on the same id
  const std::string tmp = sp.queueTmp() + "/" +
      finalPath.substr(finalPath.rfind('/') + 1) + ".w" +
      std::to_string(getpid());
  if (!env.write(tmp, err))
    return false;
  if (!commitFile(tmp, finalPath, err))
  {
    unlink(tmp.c_str());
    return false;
  }
  return true;
}

static bool movePair(const Spool& sp, const std::string& fromDir,
                     const std::string& toDir, const std::string& id,
                     const Envelope* annotated, std::string& err)
{
  // object first (idempotent; tolerate a missing object so half a
  // message can still be moved)
  const std::string srcDcm = dcmPath(fromDir, id);
  struct stat st;
  const bool haveDcm = (stat(srcDcm.c_str(), &st) == 0);
  if (haveDcm && !linkIdempotent(srcDcm, dcmPath(toDir, id), err))
    return false;

  // envelope last: its appearance commits the message in toDir
  const std::string srcEnv = envPath(fromDir, id);
  if (annotated)
  {
    if (!writeEnvelopeCommitted(sp, *annotated, envPath(toDir, id), err))
      return false;
  }
  else
  {
    const std::string tmp = sp.queueTmp() + "/" + id + ".env.w" +
        std::to_string(getpid());
    if (!copyFile(srcEnv, tmp, err))
    {
      if (!haveDcm)
        return false;  // neither half exists: nothing to move
      // object moved but envelope unreadable: leave the orphan .dcm in
      // toDir for dicomq-clean and report the envelope problem
      return false;
    }
    if (!commitFile(tmp, envPath(toDir, id), err))
    {
      unlink(tmp.c_str());
      return false;
    }
  }

  // out of the source queue: envelope first, object last
  return removePair(fromDir, id, err);
}

bool movePairRaw(const Spool& sp, const std::string& fromDir,
                 const std::string& toDir, const std::string& id,
                 std::string& err)
{
  return movePair(sp, fromDir, toDir, id, nullptr, err);
}

bool movePairAnnotated(const Spool& sp, const std::string& fromDir,
                       const std::string& toDir, const std::string& id,
                       const Envelope& env, std::string& err)
{
  return movePair(sp, fromDir, toDir, id, &env, err);
}

} // namespace dicomq
