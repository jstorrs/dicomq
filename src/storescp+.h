#ifndef STORESCP_PLUS_H
#define STORESCP_PLUS_H

class ImageDirManager
{

private:

  int serialCounter;
  OFString targetAETitle;
  OFString sourceAETitle;
  OFString tmpFileName;
  OFString newFileName;
  OFString tmpDir;
  OFString newDir;
  OFString root;
  
public:

  OFBool active;

  void
  addOptionGroup(OFCommandLine &cmd)
  {
    cmd.addGroup("tweaks:");
      cmd.addSubGroup("imagedir output:");
        cmd.addOption("--imagedir", "enable imagedir output mode");
  }

  void
  parseOptions(OFConsoleApplication& app, OFCommandLine& cmd)
  {
    active = OFFalse;

    cmd.beginOptionBlock();
    if (cmd.findOption("--imagedir"))
      {
#define IMAGEDIR_CHECK_CONFLICT(x) if (cmd.findOption(x)) app.checkConflict("--imagedir",x,OFTrue)
	IMAGEDIR_CHECK_CONFLICT("--timenames");
	IMAGEDIR_CHECK_CONFLICT("--sort-conc-studies");
	IMAGEDIR_CHECK_CONFLICT("--sort-on-study-uid");
	IMAGEDIR_CHECK_CONFLICT("--sort-on-patientname");
	IMAGEDIR_CHECK_CONFLICT("--exec-on-reception");
	IMAGEDIR_CHECK_CONFLICT("--exec-on-eostudy");
	IMAGEDIR_CHECK_CONFLICT("--rename-on-eostudy");
	IMAGEDIR_CHECK_CONFLICT("--eostudy-timeout");
	IMAGEDIR_CHECK_CONFLICT("--exec-sync");
#undef IMAGEDIR_CHECK_CONFLICT
	active = OFTrue;
      }
    cmd.endOptionBlock();
  }

  void
  setOutputDirectory(const OFString &outputDirectory)
  {
    root = outputDirectory;
  }
  
  void
  setAETitle(OFString& dest, const DIC_AE aetitle)
  {
    const OFString src = OFSTRING_GUARD(aetitle);
    size_t first = 0;
    size_t last = src.length();
    while ((first < last) && !isgraph(OFstatic_cast(unsigned char, src[first])))
      first++;
    while ((last > first) && !isgraph(OFstatic_cast(unsigned char, src[last - 1])))
      last--;
    dest.clear();
    for (size_t i = first; i < last; i++)
    {
      const unsigned char c = OFstatic_cast(unsigned char, src[i]);
      // '.' is excluded because it separates filename fields; this also
      // prevents "." and ".." from ever naming a destination directory
      if (isalnum(c) || (c == '-'))
        dest += OFstatic_cast(char, c);
      else
        dest += '_';
    }
    // never produce an empty path component (or a hidden/ambiguous filename field)
    if (dest.empty())
      dest = "_";
  }

  OFBool
  isReservedName(const OFString& name)
  {
    // the queue's own directory must never become a delivery destination;
    // compare case-insensitively to cover case-insensitive filesystems
    OFString lower = name;
    for (size_t i = 0; i < lower.length(); i++)
      lower[i] = OFstatic_cast(char, tolower(OFstatic_cast(unsigned char, lower[i])));
    return (lower == "tmp");
  }

  void
  setAETitles(const DIC_AE callingTitle, const DIC_AE calledTitle)
  {
    setAETitle(sourceAETitle, callingTitle);
    setAETitle(targetAETitle, calledTitle);
    OFStandard::combineDirAndFilename(tmpDir,root,"tmp");
    if (isReservedName(targetAETitle))
      OFStandard::combineDirAndFilename(newDir,root,"new");
    else
    {
      OFStandard::combineDirAndFilename(newDir,root,targetAETitle);
      if (!isValidDestination(newDir))
        OFStandard::combineDirAndFilename(newDir,root,"new");
    }
  }
  
  void
  generateFileNames(const T_DIMSE_C_StoreRQ *req) {
    // tmp/[Called AETitle].[Calling AETitle].[YYYYMMDDHHMMSSMMM].[PID].[COUNTER].[MODALITY].dcm
    // [Called AETitle]/[Calling AETitle].[YYYYMMDDHHMMSSMMM].[PID].[COUNTER].[MODALITY].dcm

    char uniquePart[64] = "";

    OFDateTime dateTime;
    dateTime.setCurrentDateTime();
    snprintf(uniquePart, sizeof(uniquePart), "%04u%02u%02u%02u%02u%02u%03u.%ld.%06d",
	     dateTime.getDate().getYear(), dateTime.getDate().getMonth(), dateTime.getDate().getDay(),
	     dateTime.getTime().getHour(), dateTime.getTime().getMinute(),dateTime.getTime().getIntSecond(), dateTime.getTime().getMilliSecond(),
	     OFstatic_cast(long, getpid()), serialCounter++);

    OFString stemName = uniquePart;
    stemName += '.';
    stemName += dcmSOPClassUIDToModality(req->AffectedSOPClassUID, "UNKNOWN");
    stemName += ".dcm";

    OFStandard::combineDirAndFilename(tmpFileName, tmpDir, targetAETitle + "." + sourceAETitle + "." + stemName);
    OFStandard::combineDirAndFilename(newFileName, newDir, sourceAETitle + "." + stemName);
  }

  void
  getTempFileName(char *dst, size_t dstsize) {
    OFStandard::strlcpy(dst, tmpFileName.c_str(), dstsize);
  }

  void
  finalizeDelivery() {
    OFStandard::renameFile(tmpFileName,newFileName);
  }

  bool
  isValidDestination(const OFString& dir)
  {
    return (OFStandard::dirExists(dir) && OFStandard::isWriteable(dir));
  }

  ImageDirManager() : 
    serialCounter(0),
    targetAETitle(""),
    sourceAETitle(""),
    tmpFileName(""),
    newFileName(""),
    tmpDir(""),
    newDir(""),
    root(""),
    active(OFFalse) { }

};

#endif // STORESCP_PLUS_H
