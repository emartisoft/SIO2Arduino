/*
 * disk_image.cpp - Handles disk images in various formats.
 *
 * Copyright (c) 2012 Whizzo Software LLC (Daniel Noguerol)
 *
 * This file is part of the SIO2Arduino project which emulates
 * Atari 8-bit SIO devices on Arduino hardware.
 *
 * SIO2Arduino is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * SIO2Arduino is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with SIO2Arduino; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/
#include "disk_image.h"
#include "config.h"

DiskImage::DiskImage() {
  m_fileRef = NULL;
}

boolean DiskImage::setFile(SdFile* file) {
  m_fileRef = file;
  m_fileSize = file->fileSize();

  // if image is valid...
  if (loadFile(file)) {
    return true;
  } else {
    m_fileRef = NULL;
    return false;
  }
}

byte DiskImage::getType() {
  return m_type;
}

unsigned long DiskImage::getSectorSize() {
  return m_sectorSize;
}

/**
 * Read data from drive image.
 */
SectorPacket* DiskImage::getSectorData(unsigned long sector) {
  m_sectorBuffer.length = m_sectorSize;
  m_sectorBuffer.error = false;
  m_sectorBuffer.validStatusFrame = false;

  // seek to proper offset in file
  if (m_type == TYPE_PRO) {
    // if this is a PRO image, we seek based on the sector number + the sector header size (omitting the header)
    m_fileRef->seekSet(m_headerSize + ((sector - 1) * (m_sectorSize + sizeof(PROSectorHeader))));

    // then we read the sector header
    for (int i=0; i < sizeof(PROSectorHeader); i++) {
      ((byte*)&m_proSectorHeader)[i] = (byte)m_fileRef->read();
    }

    // return the status frame so the drive can return it on a subsequent status command
    memcpy(&m_sectorBuffer.statusFrame, &m_proSectorHeader, sizeof(m_sectorBuffer.statusFrame));
    m_sectorBuffer.validStatusFrame = true;
    
    // if the header shows there was an error in this sector, return an error
    if (!m_proSectorHeader.statusFrame.hardwareStatus.crcError || !m_proSectorHeader.statusFrame.hardwareStatus.dataLostOrTrack0 || !m_proSectorHeader.statusFrame.hardwareStatus.recordNotFound) {
      m_sectorBuffer.error = true;
    } else {
      // if there are phantom sector(s) associated with this sector, decide what to return
      if (m_usePhantoms && m_proSectorHeader.totalPhantoms > 0 && m_phantomFlip) {
        m_fileRef->seekSet(m_headerSize + (((720 + m_proSectorHeader.phantom1) - 1) * (m_sectorSize + sizeof(PROSectorHeader))) + sizeof(PROSectorHeader));
      }
    }
    m_phantomFlip = !m_phantomFlip; // TODO: do bad sectors cause this to flip?
#ifdef ATX_IMAGES
  } else if (m_type == TYPE_ATX) {
    int ix = -1;
    for (int i=0; i < 720; i++) {
      if (m_sectorHeaders[i].sectorNumber == (sector-1)) {
        ix = i;
        if (!m_phantomFlip) {
          break;
        }
      }
    }
    m_sectorBuffer.validStatusFrame = true;
    if (ix > -1) {
      m_fileRef->seekSet(m_sectorHeaders[ix].fileIndex);
      if (m_sectorHeaders[ix].sstatus > 0) {
        m_sectorBuffer.error = true;
      }
      // hardware status bits for floppy controller are active low, so bit flip
      *((byte*)&m_sectorBuffer.statusFrame.hardwareStatus) = ~(m_sectorHeaders[ix].sstatus);
      *((byte*)&m_sectorBuffer.statusFrame.commandStatus) = 0x10;
      *(&m_sectorBuffer.statusFrame.timeout_lsb) = 0xE0;
    } else {
      // TODO: right now we just send back a random data frame -- is this correct?
      m_fileRef->seekSet(0);
      m_sectorBuffer.error = true;
      // set the missing sector data bit (active low)
      *((byte*)&m_sectorBuffer.statusFrame.hardwareStatus) = 0xF7;
      *((byte*)&m_sectorBuffer.statusFrame.commandStatus) = 0x10;
      *(&m_sectorBuffer.statusFrame.timeout_lsb) = 0xE0;
    }
    // for now, do the same global flip of duplicate sector data as PRO
    // (alternate between duplicate sectors on successive reads)
    // TODO: this should be based on timing of sector angular position
    m_phantomFlip = !m_phantomFlip;
#endif
  } else {
    // if this is an ATR or XFD image, we seek based on the sector number (omitting the header)
    m_fileRef->seekSet(m_headerSize + ((sector - 1) * m_sectorSize));
  }

  // delay if necessary
  if (m_sectorReadDelay) {
    delay(m_sectorReadDelay);
  }

  // read sector data into buffer
  for (int i=0; i < m_sectorSize; i++) {
    m_sectorBuffer.data[i] = (byte)m_fileRef->read();
  }

  return &m_sectorBuffer;
}

/**
 * Write data to drive image.
 */
unsigned long DiskImage::writeSectorData(unsigned long sector, byte* data, unsigned long len) {
  if (!m_readOnly) {
    // seek to proper offset in file
    unsigned long offset = m_headerSize + ((sector - 1) * m_sectorSize);
    m_fileRef->seekSet(offset);
  
    // write the data
    return m_fileRef->write(data, len);
  }
  
  return false;
}

/**
 * Format drive image.
 */
boolean DiskImage::format(SdFile *file, int density) {
  if (!m_readOnly) {
    // determine file length
    unsigned long length = FORMAT_SS_SD_40;
    if (density == DENSITY_ED) {
      length = FORMAT_SS_ED_40;
    }
  
    // make sure we're at beginning of file
    file->seekSet(0);
  
    // if disk is an ATR, write the header
    if (m_type == TYPE_ATR) {
      ATRHeader header;
      memset(&header, 0, sizeof(header));
      header.signature = ATR_SIGNATURE;
      header.pars = length / 0x10;
      header.secSize = SECTOR_SIZE_SD;
      file->write((byte*)&header, sizeof(header));
    }
  
    // create empty byte buffer
    for (unsigned long i=0; i < length; i++) {
      file->write((byte)0);
    }
  
    return true;
  }
  
  return false;
}

boolean DiskImage::loadFile(SdFile *file) {
  char filename[13];
  
  // make sure we're at the beginning of file
  file->seekSet(0);
  
  // read first 16 bytes of file & rewind again
  byte header[16];
  for (int i=0; i < 16; i++) {
    header[i] = (byte)file->read();
  }
  file->seekSet(0);
  
  // check if it's an ATR
  ATRHeader* atrHeader = (ATRHeader*)&header;
  if (atrHeader->signature == ATR_SIGNATURE) {
    m_type = TYPE_ATR;
    m_headerSize = 16;
    m_readOnly = false;
    m_sectorSize = atrHeader->secSize;
    m_sectorReadDelay = 0;
    
    LOG_MSG("Loaded ATR with sector size ");
    LOG_MSG(atrHeader->secSize);
    LOG_MSG(": ");
    
    return true;
  }

  // check if it's an APE PRO image
  PROFileHeader* proHeader = (PROFileHeader*)&header;
  if (proHeader->sectorCountHi * 256 + proHeader->sectorCountLo == ((m_fileSize-16)/(SECTOR_SIZE_SD+sizeof(PROSectorHeader))) && proHeader->magic == 'P') {
    m_type = TYPE_PRO;
    m_readOnly = true;
    m_headerSize = 16;
    m_sectorSize = SECTOR_SIZE_SD;
    m_sectorReadDelay = proHeader->sectorReadDelay * (1000/60);
    
    // set the phantom emulation mode
    switch (proHeader->phantomSectorMode) {
      case PSM_SIMPLE:
      case PSM_MINDSCAPE_SPECIAL:
      case PSM_STICKY:
      case PSM_SHIMMERING:
      case PSM_REVERSE_SHIMMER:
        m_usePhantoms = false;
        break;
      case PSM_GLOBAL_FLIP_FLOP:
        m_usePhantoms = true;
        m_phantomFlip = false;
        break;
      case PSM_GLOBAL_FLOP_FLIP:
        m_usePhantoms = true;
        m_phantomFlip = true;
        break;
    }

    LOG_MSG("Loaded PRO with sector size 128: ");

    return true;
  }

#ifdef ATX_IMAGES
  // check if it's an ATX
  if (header[0] == 'A' && header[1] == 'T' && header[2] == '8' && header[3] == 'X') {
    m_type = TYPE_ATX;
    m_readOnly = true;
    m_sectorReadDelay = 0;
    m_sectorSize = 128;
    m_phantomFlip = false;

    unsigned long trackRecordSize;
    unsigned long l2;
    unsigned long fileIndex;

    // start with all sector numbers impossibly high (for a floppy disk)
    for (int i=0; i < 720; i++) {
      m_sectorHeaders[i].sectorNumber = 60000;
    }

    // read header size
    file->seekSet(28);
    fileIndex = file->read() + file->read() * 256 + file->read() * 512 + file->read() * 768;
    // skip to first track record
    file->seekSet(fileIndex);

    // NOTE: we're doing multiple file->read() statements to avoid creating any additional
    // heap variables (we have a lot more available program space than heap space)

    for (int i=0; i < 40; i++) {
      // read track header
      trackRecordSize = file->read();
      trackRecordSize += file->read() * 256;
      trackRecordSize += file->read() * 512;
      trackRecordSize += file->read() * 768;
      file->read();
      file->read();
      file->read();
      file->read();
      byte trackNumber = file->read();
      file->read();
      int sectorCount = file->read();
      sectorCount += file->read() * 256;
      file->read();
      file->read();
      file->read();
      file->read();
      file->read();
      file->read();
      file->read();
      file->read();
      l2 = file->read();
      l2 += file->read() * 256;
      l2 += file->read() * 512;
      l2 += file->read() * 768;
      
      // seek to beginning of sector list
      file->seekSet(fileIndex + l2);
      
      // skip sector list header
      file->read();
      file->read();
      file->read();
      file->read();
      file->read();
      file->read();
      file->read();
      file->read();
      
      // read each sector
      for (int i2=0; i2< sectorCount; i2++) {
        byte sectorNum = file->read();
        byte sectorStatus = file->read();
        // skip sector position
        file->read();
        file->read();
        // read start data pos
        l2 = file->read();
        l2 += file->read() * 256;
        l2 += file->read() * 512;
        l2 += file->read() * 768;
        m_sectorHeaders[trackNumber * 18 + i2].sectorNumber = (trackNumber * 18) + (sectorNum - 1);
        m_sectorHeaders[trackNumber * 18 + i2].sstatus = sectorStatus;
        m_sectorHeaders[trackNumber * 18 + i2].fileIndex = fileIndex + l2;
      }

      // move to next track record
      fileIndex += trackRecordSize;
      file->seekSet(fileIndex);
    }

    LOG_MSG("Loaded ATX with sector size 128: ");
    return true;
  }  
#endif

  // check if it's an XFD
  // (since an XFD is just a raw data dump, we can only determine this by file name and size)
  file->getFilename((char*)&filename);
  int len = strlen(filename);
  char *extension = filename + len - 4;
  if ((!strcmp(".XFD", extension) || !strcmp(".xfd", extension)) && (m_fileSize == FORMAT_SS_SD_40)) {
    m_type = TYPE_XFD;
    m_readOnly = false;
    m_headerSize = 0;
    m_sectorSize = SECTOR_SIZE_SD;
    m_sectorReadDelay = 0;

    LOG_MSG("Loaded XFD with sector size 128: ");

    return true;
  }
  
  return false;
}

boolean DiskImage::hasImage() {
  return (m_fileRef != NULL);
}

boolean DiskImage::hasCopyProtection() {
  return (m_type == TYPE_PRO || m_type == TYPE_ATX);
}

boolean DiskImage::isEnhancedDensity() {
  return (m_fileSize == FORMAT_SS_ED_35 + m_headerSize || m_fileSize == FORMAT_SS_ED_40 + m_headerSize);
}

boolean DiskImage::isDoubleDensity() {
  return (m_fileSize == FORMAT_SS_DD_35 + m_headerSize || m_fileSize == FORMAT_SS_DD_40 + m_headerSize);
}

boolean DiskImage::isReadOnly() {
  return m_readOnly;
}

