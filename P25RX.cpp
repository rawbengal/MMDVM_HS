/*
 *   Copyright (C) 2016,2017 by Jonathan Naylor G4KLX
 *   Copyright (C) 2016,2017 by Andy Uribe CA6JAU
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "Config.h"
#include "Globals.h"
#include "P25RX.h"
#include "Utils.h"

const uint8_t SYNC_BIT_START_ERRS = 2U;
const uint8_t SYNC_BIT_RUN_ERRS   = 4U;

const unsigned int MAX_SYNC_FRAMES = 3U + 1U;

const uint8_t BIT_MASK_TABLE[] = {0x80U, 0x40U, 0x20U, 0x10U, 0x08U, 0x04U, 0x02U, 0x01U};

#define WRITE_BIT1(p,i,b) p[(i)>>3] = (b) ? (p[(i)>>3] | BIT_MASK_TABLE[(i)&7]) : (p[(i)>>3] & ~BIT_MASK_TABLE[(i)&7])

CP25RX::CP25RX() :
m_prev(false),
m_state(P25RXS_NONE),
m_bitBuffer(0x00U),
m_outBuffer(),
m_buffer(NULL),
m_bufferPtr(0U),
m_lostCount(0U)
{
  m_buffer = m_outBuffer + 1U;
}

void CP25RX::reset()
{
  m_prev      = false;
  m_state     = P25RXS_NONE;
  m_bitBuffer = 0x00U;
  m_bufferPtr = 0U;
  m_lostCount = 0U;
}

void CP25RX::databit(bool bit)
{
  if (m_state == P25RXS_NONE)
    processNone(bit);
  else
    processData(bit);
}

void CP25RX::processNone(bool bit)
{
  m_bitBuffer <<= 1;
  if (bit)
    m_bitBuffer |= 0x01U;

  // Fuzzy matching of the data sync bit sequence
  if (countBits64((m_bitBuffer & P25_SYNC_BITS_MASK) ^ P25_SYNC_BITS) <= SYNC_BIT_START_ERRS) {
    DEBUG1("P25RX: sync found in None");
    for (uint8_t i = 0U; i < P25_SYNC_LENGTH_BYTES; i++)
      m_buffer[i] = P25_SYNC_BYTES[i];
        
    m_lostCount = MAX_SYNC_FRAMES;
    m_bufferPtr = P25_SYNC_LENGTH_BITS;
    m_state     = P25RXS_DATA;
    
    io.setDecode(true);
  }
}

void CP25RX::processData(bool bit)
{
  m_bitBuffer <<= 1;
  if (bit)
    m_bitBuffer |= 0x01U;

  WRITE_BIT1(m_buffer, m_bufferPtr, bit);
  m_bufferPtr++;

  // Search for an early sync to indicate an LDU following a header
  if (m_bufferPtr >= (P25_HDR_FRAME_LENGTH_BITS + P25_SYNC_LENGTH_BITS - 1U) && m_bufferPtr <= (P25_HDR_FRAME_LENGTH_BITS + P25_SYNC_LENGTH_BITS + 1U)) {
    // Fuzzy matching of the data sync bit sequence
    if (countBits64((m_bitBuffer & P25_SYNC_BITS_MASK) ^ P25_SYNC_BITS) <= SYNC_BIT_RUN_ERRS) {
      DEBUG2("P25RX: found LDU sync in Data, pos", m_bufferPtr - P25_SYNC_LENGTH_BITS);

      m_outBuffer[0U] = 0x01U;
      serial.writeP25Hdr(m_outBuffer, P25_HDR_FRAME_LENGTH_BYTES + 1U);

      // Restore the sync that's now in the wrong place
      for (uint8_t i = 0U; i < P25_SYNC_LENGTH_BYTES; i++)
        m_buffer[i] = P25_SYNC_BYTES[i];

      m_lostCount = MAX_SYNC_FRAMES;
      m_bufferPtr = P25_SYNC_LENGTH_BITS;
    }
  }

  // Only search for a sync in the right place +-2 symbols
  if (m_bufferPtr >= (P25_SYNC_LENGTH_BITS - 2U) && m_bufferPtr <= (P25_SYNC_LENGTH_BITS + 2U)) {
    // Fuzzy matching of the data sync bit sequence
    if (countBits64((m_bitBuffer & P25_SYNC_BITS_MASK) ^ P25_SYNC_BITS) <= SYNC_BIT_RUN_ERRS) {
      DEBUG2("P25RX: found sync in Data, pos", m_bufferPtr - P25_SYNC_LENGTH_BITS);
      m_lostCount = MAX_SYNC_FRAMES;
      m_bufferPtr = P25_SYNC_LENGTH_BITS;
    }
  }

  // Send a data frame to the host if the required number of bits have been received
  if (m_bufferPtr == P25_LDU_FRAME_LENGTH_BITS) {
    // We've not seen a data sync for too long, signal RXLOST and change to RX_NONE
    m_lostCount--;
    if (m_lostCount == 0U) {
      DEBUG1("P25RX: sync timed out, lost lock");
      io.setDecode(false);
      
      serial.writeP25Lost();

      m_state = P25RXS_NONE;
    } else {
      m_outBuffer[0U] = m_lostCount == (MAX_SYNC_FRAMES - 1U) ? 0x01U : 0x00U;

      writeRSSILdu(m_outBuffer);
      
      // Start the next frame
      ::memset(m_outBuffer, 0x00U, P25_LDU_FRAME_LENGTH_BYTES + 3U);
      m_bufferPtr = 0U;
    }
  }
}

void CP25RX::writeRSSILdu(uint8_t* ldu)
{
#if defined(SEND_RSSI_DATA)
  uint16_t rssi = io.readRSSI();
  
  ldu[217U] = (rssi >> 8) & 0xFFU;
  ldu[218U] = (rssi >> 0) & 0xFFU;
  
  serial.writeP25Ldu(ldu, P25_LDU_FRAME_LENGTH_BYTES + 3U);
#else
  serial.writeP25Ldu(ldu, P25_LDU_FRAME_LENGTH_BYTES + 1U);
#endif
}


