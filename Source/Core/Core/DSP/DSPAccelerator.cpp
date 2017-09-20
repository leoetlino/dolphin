// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Core/DSP/DSPAccelerator.h"

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MathUtil.h"

namespace DSP
{
u16 Accelerator::ReadD3()
{
  u16 val = 0;

  switch (m_sample_format)
  {
  case 0x5:  // u8 reads
    val = ReadMemory(m_current_address);
    m_current_address++;
    break;
  case 0x6:  // u16 reads
    val = (ReadMemory(m_current_address * 2) << 8) | ReadMemory(m_current_address * 2 + 1);
    m_current_address++;
    break;
  default:
    ERROR_LOG(DSPLLE, "dsp_read_aram_d3() - unknown format 0x%x", m_sample_format);
    break;
  }

  if (m_current_address >= m_end_address)
  {
    // Set address back to start address. (never seen this here!)
    m_current_address = m_start_address;
  }
  return val;
}

void Accelerator::WriteD3(u16 value)
{
  // Zelda ucode writes a bunch of zeros to ARAM through d3 during
  // initialization.  Don't know if it ever does it later, too.
  // Pikmin 2 Wii writes non-stop to 0x10008000-0x1000801f (non-zero values too)
  // Zelda TP Wii writes non-stop to 0x10000000-0x1000001f (non-zero values too)

  switch (m_sample_format)
  {
  case 0xA:  // u16 writes
    WriteMemory(m_current_address * 2, value >> 8);
    WriteMemory(m_current_address * 2 + 1, value & 0xFF);
    m_current_address++;
    break;
  default:
    ERROR_LOG(DSPLLE, "dsp_write_aram_d3() - unknown format 0x%x", m_sample_format);
    break;
  }
}

u16 Accelerator::Read(s16* coefs)
{
  u16 val;
  u8 step_size_bytes = 0;

  // let's do the "hardware" decode DSP_FORMAT is interesting - the Zelda
  // ucode seems to indicate that the bottom two bits specify the "read size"
  // and the address multiplier.  The bits above that may be things like sign
  // extension and do/do not use ADPCM.  It also remains to be figured out
  // whether there's a difference between the usual accelerator "read
  // address" and 0xd3.
  switch (m_sample_format)
  {
  case 0x00:  // ADPCM audio
  {
    // ADPCM decoding, not much to explain here.
    if ((m_current_address & 15) == 0)
    {
      m_pred_scale = ReadMemory((m_current_address & ~15) >> 1);
      m_current_address += 2;
    }

    switch (m_end_address & 15)
    {
    case 0:  // Tom and Jerry
      step_size_bytes = 1;
      break;
    case 1:  // Blazing Angels
      step_size_bytes = 0;
      break;
    default:
      step_size_bytes = 2;
      break;
    }

    int scale = 1 << (m_pred_scale & 0xF);
    int coef_idx = (m_pred_scale >> 4) & 0x7;

    s32 coef1 = coefs[coef_idx * 2 + 0];
    s32 coef2 = coefs[coef_idx * 2 + 1];

    int temp = (m_current_address & 1) ? (ReadMemory(m_current_address >> 1) & 0xF) :
                                         (ReadMemory(m_current_address >> 1) >> 4);

    if (temp >= 8)
      temp -= 16;

    s32 val32 = (scale * temp) + ((0x400 + coef1 * m_yn1 + coef2 * m_yn2) >> 11);
    val = static_cast<s16>(MathUtil::Clamp<s32>(val32, -0x7FFF, 0x7FFF));

    m_yn2 = m_yn1;
    m_yn1 = val;
    m_current_address += 1;
    break;
  }
  case 0x0A:  // 16-bit PCM audio
    val = (ReadMemory(m_current_address * 2) << 8) | ReadMemory(m_current_address * 2 + 1);
    m_yn2 = m_yn1;
    m_yn1 = val;
    step_size_bytes = 2;
    m_current_address += 1;
    break;
  case 0x19:  // 8-bit PCM audio
    val = ReadMemory(m_current_address) << 8;
    m_yn2 = m_yn1;
    m_yn1 = val;
    step_size_bytes = 2;
    m_current_address += 1;
    break;
  default:
    ERROR_LOG(DSPLLE, "dsp_read_accelerator() - unknown format 0x%x", m_sample_format);
    step_size_bytes = 2;
    m_current_address += 1;
    val = 0;
    break;
  }

  // TODO: Take GAIN into account
  // adpcm = 0, pcm8 = 0x100, pcm16 = 0x800
  // games using pcm8 : Phoenix Wright Ace Attorney (WiiWare), Megaman 9-10 (WiiWare)
  // games using pcm16: GC Sega games, ...

  // Check for loop.
  // Somehow, YN1 and YN2 must be initialized with their "loop" values,
  // so yeah, it seems likely that we should raise an exception to let
  // the DSP program do that, at least if DSP_FORMAT == 0x0A.
  if (m_current_address == (m_end_address + step_size_bytes - 1))
  {
    // Set address back to start address.
    m_current_address = m_start_address;
    OnEndException();
  }

  SetCurrentAddress(m_current_address);
  return val;
}

void Accelerator::DoState(PointerWrap& p)
{
  p.Do(m_start_address);
  p.Do(m_end_address);
  p.Do(m_current_address);
  p.Do(m_sample_format);
  p.Do(m_yn1);
  p.Do(m_yn2);
  p.Do(m_pred_scale);
}

constexpr u32 START_END_ADDRESS_MASK = 0x3fffffff;
constexpr u32 CURRENT_ADDRESS_MASK = 0xbfffffff;

void Accelerator::SetStartAddress(u32 address)
{
  m_start_address = address & START_END_ADDRESS_MASK;
  printf("[accelerator] start=%08x\n", m_start_address);
}

void Accelerator::SetEndAddress(u32 address)
{
  m_end_address = address & START_END_ADDRESS_MASK;
  printf("[accelerator] end=%08x\n", m_end_address);
}

void Accelerator::SetCurrentAddress(u32 address)
{
  m_current_address = address & CURRENT_ADDRESS_MASK;
}

void Accelerator::SetSampleFormat(u16 format)
{
  m_sample_format = format;
}

void Accelerator::SetYn1(s16 yn1)
{
  m_yn1 = yn1;
}

void Accelerator::SetYn2(s16 yn2)
{
  m_yn2 = yn2;
}

void Accelerator::SetPredScale(u16 pred_scale)
{
  m_pred_scale = pred_scale;
}
}  // namespace DSP
