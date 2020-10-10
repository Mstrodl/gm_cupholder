#include <GarrysMod/Lua/Interface.h>
#ifdef __linux__
#include <sys/ioctl.h>
#include <linux/cdrom.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include <scsi/sg.h>
#include <scsi/scsi.h>
#include <scsi/scsi_ioctl.h>
#include <memory.h>
#include <linux/fs.h>

// TODO: REMOVE
#include <stdio.h>

#define DEFAULT_DEVICE "/dev/cdrom"
#elif defined(_WIN32)
#include <fileapi.h>
#define DEFAULT_DEVICE formatLabel(defaultCDWindows())
#endif

#ifdef _WIN32
char* formatLabel(char* label) {
  const char[7] path = {'\\', '\\', '.', '\\', label[0], ':', 0};
  return path;
}

char* defaultCDWindows() {
  for(char label = 'A'; label < 'Z'; ++label) {
    const UINT type = GetDriveTypeA(label);
    if(type == DRIVE_CDROM) {
      const char[2] str = {label, 0};
      return str;
    }
  }
}
#endif

LUA_FUNCTION(check)
{
  const char* device = LUA->Top() >= 1 ? LUA->GetString(1) : DEFAULT_DEVICE;
  int state = -1;
#ifdef __linux__
  const int fd = open(device, O_RDWR | O_NONBLOCK);
  printf("Opening %s got us FD %d\n", device, fd);
  if(fd > 0) {
    const int status = ioctl(fd, CDROM_DRIVE_STATUS);
    printf("Current status is %d\n", status);
    switch(status) {
    case CDS_TRAY_OPEN: {
      state = 1;
      break;
    }
    case CDS_NO_DISC:
    case CDS_DISC_OK: {
      state = 0;
      break;
    }
    }
  }
#endif
  // 0 = closed
  // 1 = open
  // -1 = unknown
  LUA->PushNumber(state);
  return 1;
}

#ifdef __linux__
static int eject_scsi(const int fd)
{
  int status, k;
  sg_io_hdr_t io_hdr;
  unsigned char allowRmBlk[6] = {ALLOW_MEDIUM_REMOVAL, 0, 0, 0, 0, 0};
  unsigned char startStop1Blk[6] = {START_STOP, 0, 0, 0, 1, 0};
  unsigned char startStop2Blk[6] = {START_STOP, 0, 0, 0, 2, 0};
  unsigned char inqBuff[2];
  unsigned char sense_buffer[32];

  if ((ioctl(fd, SG_GET_VERSION_NUM, &k) < 0) || (k < 30000)) {
    printf("Not an sg device, or old sg driver\n");
    return 0;
  }

  memset(&io_hdr, 0, sizeof(sg_io_hdr_t));
  io_hdr.interface_id = 'S';
  io_hdr.cmd_len = 6;
  io_hdr.mx_sb_len = sizeof(sense_buffer);
  io_hdr.dxfer_direction = SG_DXFER_NONE;
  io_hdr.dxfer_len = 0;
  io_hdr.dxferp = inqBuff;
  io_hdr.sbp = sense_buffer;
  io_hdr.timeout = 10000;

  io_hdr.cmdp = allowRmBlk;
  status = ioctl(fd, SG_IO, (void *)&io_hdr);
  if (status < 0 || io_hdr.host_status || io_hdr.driver_status) {
    return 0;
  }

  io_hdr.cmdp = startStop1Blk;
  status = ioctl(fd, SG_IO, (void *)&io_hdr);
  if (status < 0 || io_hdr.host_status) {
    return 0;
  }

  /* Ignore errors when there is not medium -- in this case driver sense
   * buffer sets MEDIUM NOT PRESENT (3a) bit. For more details see:
   * http://www.tldp.org/HOWTO/archived/SCSI-Programming-HOWTO/SCSI-Programming-HOWTO-22.html#sec-sensecodes
   * -- kzak Jun 2013
   */
  if (io_hdr.driver_status != 0 &&
      !(io_hdr.driver_status == /*SG_ERR_DRIVER_SENSE*/ 8 && io_hdr.sbp &&
	io_hdr.sbp[12] == 0x3a)) {
    return 0;
  }

  io_hdr.cmdp = startStop2Blk;
  status = ioctl(fd, SG_IO, (void *)&io_hdr);
  if (status < 0 || io_hdr.host_status || io_hdr.driver_status) {
    return 0;
  }

  /* force kernel to reread partition table when new disc inserted */
  ioctl(fd, BLKRRPART);
  return 1;
}
#endif

LUA_FUNCTION(set)
{
  const bool hasDevice = LUA->Top() >= 2;
  const bool isOpen = LUA->GetBool(1);
  const char* device = hasDevice ? LUA->GetString(2) : DEFAULT_DEVICE;

  bool success = 0;
#ifdef __linux__
  const int fd = open(device, O_RDWR | O_NONBLOCK);
  printf("Opening %s gave us FD %d\n", device, fd);
  if(fd > 0) {
    if(isOpen) {
#ifdef CDROMEJECT
      printf("Using CDROMEJECT\n");
      success = ioctl(fd, CDROMEJECT) != -1;
#elif defined(CDIOCEJECT)
      printf("Using CDIOCEJECT\n");
      success = ioctl(fd, CDIOCEJECT) != -1;
#endif

      if(!success) {
	success = eject_scsi(fd);
      }
    } else {
#ifdef CDROMCLOSETRAY
      success = ioctl(fd, CDROMCLOSETRAY) != -1;
#elif defined(CDIOCCLOSE)
      success = ioctl(fd, CDIOCCLOSE) != -1;
#endif
    }

    if(!success) {
      printf("Err: %d\n", errno);
    }
  }
#elif defined(_WIN32)
  const HANDLE fd = CreateFile(_T(formatLabel(device)), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
  if(fd != INVALID_HANDLE_VALUE) {
    DWORD bytes;
    if(isOpen) {
      DeviceIoControl(fd, IOCTL_STORAGE_EJECT_MEDIA, NULL, 0, NULL, 0, &bytes, NULL);
    } else {
      DeviceIoControl(fd, IOCTL_STORAGE_LOAD_MEDIA, NULL, 0, NULL, 0, &bytes, NULL);
    }
    CloseHandle(fd);
  }
#endif

  LUA->PushBool(success);
  return 1;
}

GMOD_MODULE_OPEN()
{
  LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);

  LUA->CreateTable();
    LUA->PushCFunction(check);
    LUA->SetField(-2, "check");
    LUA->PushCFunction(set);
    LUA->SetField(-2, "set");
  LUA->SetField(-2, "cupholder");

  LUA->Pop();

  return 0;
}

GMOD_MODULE_CLOSE()
{
  return 0;
}
