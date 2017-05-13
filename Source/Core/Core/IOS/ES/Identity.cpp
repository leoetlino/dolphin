// Copyright 2017 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Core/IOS/ES/ES.h"

#include <cstring>
#include <vector>

#include "Common/Assert.h"
#include "Common/Logging/Log.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/ES/Formats.h"
#include "Core/ec_wii.h"

namespace IOS
{
namespace HLE
{
namespace Device
{
ReturnCode ES::GetDeviceId(u32* device_id)
{
  *device_id = EcWii::GetInstance().GetNGID();
  INFO_LOG(IOS_ES, "GetDeviceId: %08X", *device_id);
  return IPC_SUCCESS;
}

IPCCommandResult ES::GetDeviceId(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(0, 1))
    return GetDefaultReply(ES_EINVAL);

  u32 device_id;
  GetDeviceId(&device_id);
  Memory::Write_U32(device_id, request.io_vectors[0].address);
  return GetDefaultReply(IPC_SUCCESS);
}

ReturnCode ES::Encrypt(u32 uid, u32 encrypt_handle, u8 *iv, const u8 *input, u32 size, u8 *output)
{
  // TODO: Check whether the active title is allowed to encrypt.
  const ReturnCode ret = m_ios.GetIOSC().Encrypt(encrypt_handle, iv, input, size, output, PID_ES);
  return ret;
}

IPCCommandResult ES::Encrypt(u32 uid, const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(3, 2))
    return GetDefaultReply(ES_EINVAL);

  const u32 encrypt_handle = Memory::Read_U32(request.in_vectors[0].address);
  const u8* input = Memory::GetPointer(request.in_vectors[2].address);
  const u32 size = request.in_vectors[2].size;
  u8* iv = Memory::GetPointer(request.io_vectors[0].address);
  u8* output = Memory::GetPointer(request.io_vectors[1].address);

  return GetDefaultReply(Encrypt(uid, encrypt_handle, iv, input, size, output));
}

ReturnCode ES::Decrypt(u32 uid, u32 decrypt_handle, u8 *iv, const u8 *input, u32 size, u8 *output)
{
  // TODO: Check whether the active title is allowed to decrypt.
  const ReturnCode ret = m_ios.GetIOSC().Decrypt(decrypt_handle, iv, input, size, output, PID_ES);
  return ret;
}

IPCCommandResult ES::Decrypt(u32 uid, const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(3, 2))
    return GetDefaultReply(ES_EINVAL);

  const u32 decrypt_handle = Memory::Read_U32(request.in_vectors[0].address);
  const u8* input = Memory::GetPointer(request.in_vectors[2].address);
  const u32 size = request.in_vectors[2].size;
  u8* iv = Memory::GetPointer(request.io_vectors[0].address);
  u8* output = Memory::GetPointer(request.io_vectors[1].address);

  return GetDefaultReply(Decrypt(uid, decrypt_handle, iv, input, size, output));
}

ReturnCode ES::CheckKoreaRegion()
{
  // note by DacoTaco : name is unknown, I just tried to name it SOMETHING.
  // IOS70 has this to let system menu 4.2 check if the console is region changed. it returns
  // -1017
  // if the IOS didn't find the Korean keys and 0 if it does. 0 leads to a error 003
  INFO_LOG(IOS_ES, "IOCTL_ES_CHECKKOREAREGION: Title checked for Korean keys.");
  return ES_EINVAL;
}

IPCCommandResult ES::CheckKoreaRegion(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(0, 0))
    return GetDefaultReply(ES_EINVAL);
  return GetDefaultReply(CheckKoreaRegion());
}

ReturnCode ES::GetDeviceCert(u8 *cert)
{
  INFO_LOG(IOS_ES, "GetDeviceCert");

  const EcWii& ec = EcWii::GetInstance();
  MakeNGCert(cert, ec.GetNGID(), ec.GetNGKeyID(), ec.GetNGPriv(), ec.GetNGSig());
  return IPC_SUCCESS;
}

IPCCommandResult ES::GetDeviceCert(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(0, 1) || request.io_vectors[0].size != 0x180)
    return GetDefaultReply(ES_EINVAL);

  u8* certificate = Memory::GetPointer(request.io_vectors[0].address);
  return GetDefaultReply(GetDeviceCert(certificate));
}

ReturnCode ES::Sign(const u8* data, u32 data_size, u8 *signature, u8 *ecc_certificate)
{
  INFO_LOG(IOS_ES, "Sign");
  if (!GetTitleContext().active)
    return ES_EINVAL;

  const EcWii& ec = EcWii::GetInstance();
  MakeAPSigAndCert(signature, ecc_certificate, GetTitleContext().tmd.GetTitleId(), data, data_size,
                   ec.GetNGPriv(), ec.GetNGID());

  return IPC_SUCCESS;
}

IPCCommandResult ES::Sign(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 2))
    return GetDefaultReply(ES_EINVAL);

  const u8* data = Memory::GetPointer(request.in_vectors[0].address);
  const u32 data_size = request.in_vectors[0].size;
  u8* signature = Memory::GetPointer(request.io_vectors[0].address);
  u8* ecc_certificate = Memory::GetPointer(request.io_vectors[1].address);

  return GetDefaultReply(Sign(data, data_size, signature, ecc_certificate));
}
}  // namespace Device
}  // namespace HLE
}  // namespace IOS
