// Copyright 2018 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Core/IOS/FS/ImageBackend/FS.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>

#include "Common/Align.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Core/IOS/FS/ImageBackend/ECC.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/IOSC.h"
#include "Core/IOS/Uids.h"

namespace IOS::HLE::FS
{
IOSC::Hash NandFileSystem::GenerateHmacForSuperblock(const Superblock& superblock, u16 index)
{
  SuperblockSalt salt{};
  salt.starting_cluster = SuperblockCluster(index);
  m_block_mac_generator.Update(reinterpret_cast<u8*>(&salt), sizeof(salt));
  m_block_mac_generator.Update(reinterpret_cast<const u8*>(&superblock), sizeof(superblock));
  return m_block_mac_generator.FinaliseAndGetHash();
}

IOSC::Hash NandFileSystem::GenerateHmacForData(const Superblock& superblock, const u8* cluster_data,
                                               u16 fst_index, u16 chain_index)
{
  const FstEntry& entry = superblock.fst.at(fst_index);
  DataSalt salt{};
  salt.uid = entry.uid;
  salt.name = entry.name;
  salt.chain_index = chain_index;
  salt.fst_index = fst_index;
  salt.x3 = entry.x3;

  m_block_mac_generator.Update(reinterpret_cast<u8*>(&salt), sizeof(salt));
  m_block_mac_generator.Update(cluster_data, CLUSTER_DATA_SIZE);
  return m_block_mac_generator.FinaliseAndGetHash();
}

Result<std::array<IOSC::Hash, 2>> NandFileSystem::ReadCluster(u16 cluster, u8* result)
{
  if (cluster >= 0x8000)
    return ResultCode::Invalid;

  DEBUG_LOG(IOS_FS, "Reading cluster 0x%04x", cluster);
  std::array<IOSC::Hash, 2> hmacs;
  for (u32 page = 0; page < PAGES_PER_CLUSTER; ++page)
  {
    const s64 offset = Offset(cluster, page);
    if (!m_nand.Seek(offset, SEEK_SET) ||
        !m_nand.ReadBytes(result + DATA_BYTES_PER_PAGE * page, DATA_BYTES_PER_PAGE))
    {
      return ResultCode::BadBlock;
    }
    if (!m_nand.Seek(1, SEEK_CUR) ||
        (page == HMAC_PAGE1 && (!m_nand.ReadBytes(hmacs[0].data(), HMAC1_SIZE_IN_PAGE1) ||
                                !m_nand.ReadBytes(hmacs[1].data(), HMAC2_SIZE_IN_PAGE1))) ||
        (page == HMAC_PAGE2 &&
         !m_nand.ReadBytes(hmacs[1].data() + HMAC2_SIZE_IN_PAGE1, HMAC2_SIZE_IN_PAGE2)))
    {
      return ResultCode::BadBlock;
    }
  }

  if (cluster < SUPERBLOCK_START_CLUSTER)
  {
    std::array<u8, 16> iv{};
    m_iosc.Decrypt(IOSC::HANDLE_FS_KEY, iv.data(), result, CLUSTER_DATA_SIZE, result, PID_FS);
  }

  return hmacs;
}

ResultCode NandFileSystem::WriteCluster(u16 cluster, const u8* data, const IOSC::Hash& hmac)
{
  if (cluster >= 0x8000)
    return ResultCode::Invalid;

  DEBUG_LOG(IOS_FS, "Writing to cluster 0x%04x", cluster);
  std::array<u8, 16> iv{};
  std::vector<u8> data_to_write(DATA_BYTES_PER_PAGE);
  for (u32 page = 0; page < PAGES_PER_CLUSTER; ++page)
  {
    const u8* source = &data[page * DATA_BYTES_PER_PAGE];

    // Write the page data.
    if (cluster >= SUPERBLOCK_START_CLUSTER)
    {
      std::copy_n(source, DATA_BYTES_PER_PAGE, data_to_write.begin());
    }
    else
    {
      m_iosc.Encrypt(IOSC::HANDLE_FS_KEY, iv.data(), source, DATA_BYTES_PER_PAGE,
                     data_to_write.data(), PID_FS);
    }

    if (!m_nand.Seek(Offset(cluster, page), SEEK_SET) ||
        !m_nand.WriteBytes(data_to_write.data(), data_to_write.size()))
    {
      return ResultCode::BadBlock;
    }

    // Write the spare data (ECC / HMAC).
    std::array<u8, 0x40> spare{};
    spare[0] = 0xff;
    const EccData ecc = CalculateEcc(data_to_write.data());
    std::copy(ecc.begin(), ecc.end(), &spare[0x30]);
    if (page == HMAC_PAGE1)
    {
      std::copy(hmac.begin(), hmac.end(), &spare[HMAC1_OFFSET_IN_PAGE1]);
      // Second, partial copy of the HMAC.
      std::copy_n(hmac.data(), HMAC2_SIZE_IN_PAGE1, &spare[HMAC2_OFFSET_IN_PAGE1]);
    }
    else if (page == HMAC_PAGE2)
    {
      // Copy the rest of the HMAC.
      std::copy_n(hmac.data() + HMAC2_SIZE_IN_PAGE1, HMAC2_SIZE_IN_PAGE2,
                  &spare[HMAC2_OFFSET_IN_PAGE2]);
    }

    // Write the spare data.
    if (!m_nand.WriteBytes(spare.data(), spare.size()))
      return ResultCode::BadBlock;
  }

  return ResultCode::Success;
}

static std::optional<u16> GetClusterForFile(const Superblock& superblock, const u16 first_cluster,
                                            size_t index)
{
  u16 cluster = first_cluster;
  for (size_t i = 0; i < index; ++i)
  {
    if (cluster >= superblock.fat.size())
    {
      WARN_LOG(IOS_FS, "Cannot find cluster number with index %zu in chain 0x%04x\n", index,
               first_cluster);
      return {};
    }
    cluster = superblock.fat[cluster];
  }
  if (cluster >= superblock.fat.size())
    return {};
  return cluster;
}

ResultCode NandFileSystem::WriteFileData(u16 fst_index, const u8* source, u16 chain_index,
                                         u32 new_size)
{
  if (fst_index >= std::tuple_size<decltype(Superblock::fst)>::value)
    return ResultCode::Invalid;

  auto* superblock = GetSuperblock();
  if (!superblock)
    return ResultCode::SuperblockInitFailed;

  FstEntry& entry = superblock->fst[fst_index];
  if (!entry.IsFile() || new_size < entry.size)
    return ResultCode::Invalid;

  // Currently, clusters are allocated in a very simple way that ignores wear leveling
  // since we are not writing to an actual flash device anyway.
  const auto it = std::find(superblock->fat.begin(), superblock->fat.end(), CLUSTER_UNUSED);
  if (it == superblock->fat.end())
    return ResultCode::NoFreeSpace;
  const u16 cluster = it - superblock->fat.begin();

  const auto hash = GenerateHmacForData(*superblock, source, fst_index, chain_index);
  const auto write_result = WriteCluster(cluster, source, hash);
  if (write_result != ResultCode::Success)
    return write_result;

  const std::optional<u16> old_cluster = GetClusterForFile(*superblock, entry.sub, chain_index);

  // Change the previous cluster (or the FST) to point to the new cluster
  if (chain_index == 0)
  {
    entry.sub = cluster;
  }
  else
  {
    const std::optional<u16> prev = GetClusterForFile(*superblock, entry.sub, chain_index - 1);
    if (!prev)
      return ResultCode::Invalid;
    superblock->fat[*prev] = cluster;
  }

  // If we are replacing another cluster, keep pointing to the same next cluster
  if (old_cluster)
    superblock->fat[cluster] = superblock->fat[*old_cluster];
  else
    superblock->fat[cluster] = CLUSTER_LAST_IN_CHAIN;

  // Free the old cluster now
  if (old_cluster)
    superblock->fat[*old_cluster] = CLUSTER_UNUSED;

  entry.size = new_size;
  return ResultCode::Success;
}

Result<Superblock> NandFileSystem::ReadSuperblock(u16 superblock)
{
  Superblock block;
  for (u32 i = 0; i < CLUSTERS_PER_SUPERBLOCK; ++i)
  {
    const auto result = ReadCluster(SuperblockCluster(superblock) + i,
                                   reinterpret_cast<u8*>(&block) + CLUSTER_DATA_SIZE * i);
    if (!result)
      return result.Error();
  }
  return block;
}

ResultCode NandFileSystem::ReadFileData(u16 fst_index, u16 chain_index, u8* data)
{
  if (fst_index >= std::tuple_size<decltype(Superblock::fst)>::value)
    return ResultCode::Invalid;

  const auto* superblock = GetSuperblock();
  if (!superblock)
    return ResultCode::SuperblockInitFailed;

  const FstEntry& entry = superblock->fst[fst_index];
  if (!entry.IsFile() || entry.size <= chain_index * CLUSTER_DATA_SIZE)
    return ResultCode::Invalid;

  const auto hmacs = ReadCluster(*GetClusterForFile(*superblock, entry.sub, chain_index), data);
  if (!hmacs)
    return hmacs.Error();

  const auto hash = GenerateHmacForData(*superblock, data, fst_index, chain_index);
  if (hash != (*hmacs)[0] && hash != (*hmacs)[1])
  {
    ERROR_LOG(IOS_FS, "Failed to verify cluster data (fst_index 0x%04x chain_index %u)", fst_index,
              chain_index);
    return ResultCode::CheckFailed;
  }

  return ResultCode::Success;
}

Superblock* NandFileSystem::GetSuperblock()
{
  if (m_superblock)
    return m_superblock.get();

  u32 highest_version = 0;
  for (u32 i = 0; i < NUMBER_OF_SUPERBLOCKS; ++i)
  {
    const Result<Superblock> superblock = ReadSuperblock(i);
    if (!superblock || superblock->magic != SUPERBLOCK_MAGIC)
      continue;

    if (superblock->version < highest_version)
      continue;

    highest_version = superblock->version;
    m_superblock_index = i;
    m_superblock = std::make_unique<Superblock>(*superblock);
  }

  if (!m_superblock)
    return nullptr;

  INFO_LOG(IOS_FS, "Using superblock %u (v%u)", m_superblock_index, u32(m_superblock->version));

  const auto hash = GenerateHmacForSuperblock(*m_superblock, m_superblock_index);
  std::vector<u8> buffer(CLUSTER_DATA_SIZE);
  const auto hmacs = ReadCluster(SuperblockCluster(m_superblock_index) + 15, buffer.data());
  if (!hmacs || (hash != (*hmacs)[0] && hash != (*hmacs)[1]))
  {
    ERROR_LOG(IOS_FS, "Failed to verify superblock");
    return nullptr;
  }

  return m_superblock.get();
}

ResultCode NandFileSystem::FlushSuperblock()
{
  if (!m_superblock)
    return ResultCode::NotFound;

  m_superblock->version = m_superblock->version + 1;

  const auto write_block = [this]() {
    m_superblock_index = (m_superblock_index + 1) % NUMBER_OF_SUPERBLOCKS;
    const auto hmac = GenerateHmacForSuperblock(*m_superblock, m_superblock_index);
    const IOSC::Hash null_hmac{};

    for (u32 cluster = 0, offset = 0; cluster < CLUSTERS_PER_SUPERBLOCK; ++cluster)
    {
      const ResultCode result = WriteCluster(SuperblockCluster(m_superblock_index) + cluster,
                                             reinterpret_cast<u8*>(m_superblock.get()) + offset,
                                             cluster == 15 ? hmac : null_hmac);
      if (result != ResultCode::Success)
        return result;

      static_assert(CLUSTERS_PER_SUPERBLOCK * CLUSTER_DATA_SIZE == sizeof(Superblock));
      offset += CLUSTER_DATA_SIZE;
    }

    // According to WiiQt/nandbin, 15 other versions should be written after an overflow
    // so that the driver doesn't pick an older superblock.
    if (m_superblock->version == 0)
    {
      WARN_LOG(IOS_FS, "Superblock version overflowed -- writing 15 extra versions");
      for (int i = 0; i < 15; ++i)
      {
        const ResultCode result = FlushSuperblock();
        if (result != ResultCode::Success)
          return result;
      }
    }
    return ResultCode::Success;
  };

  for (u32 i = 0; i < NUMBER_OF_SUPERBLOCKS; ++i)
  {
    if (write_block() == ResultCode::Success)
      return ResultCode::Success;
    ERROR_LOG(IOS_FS, "Failed to write superblock at index %d", i);
  }
  ERROR_LOG(IOS_FS, "Failed to flush superblock");
  return ResultCode::SuperblockWriteFailed;
}

Result<u16> NandFileSystem::GetFstIndex(const Superblock& superblock, const std::string& path) const
{
  if (path == "/" || path.empty())
    return 0;

  u16 fst_index = 0;
  for (const auto& component : SplitString(path.substr(1), '/'))
  {
    const Result<u16> result = GetFstIndex(superblock, fst_index, component);
    if (!result || *result >= superblock.fst.size())
      return ResultCode::Invalid;
    fst_index = *result;
  }
  return fst_index;
}

Result<u16> NandFileSystem::GetFstIndex(const Superblock& superblock, u16 parent,
                                        const std::string& file_name) const
{
  if (parent >= superblock.fst.size() || file_name.size() > 12)
    return ResultCode::Invalid;

  // Traverse the tree until we find a match or there are no more children
  u16 index = superblock.fst[parent].sub;
  if (index >= superblock.fst.size())
    return ResultCode::Invalid;

  do
  {
    if (superblock.fst[index].GetName() == file_name)
      return index;
    index = superblock.fst[index].sib;
  } while (index < superblock.fst.size());
  return ResultCode::Invalid;
}

Result<u16> NandFileSystem::GetUnusedFstIndex(const Superblock& superblock) const
{
  auto it = std::find_if(superblock.fst.begin(), superblock.fst.end(),
                         [](const FstEntry& entry) { return (entry.mode & 3) == 0; });
  if (it == superblock.fst.end())
    return ResultCode::FstFull;
  return it - superblock.fst.begin();
}

}  // namespace IOS::HLE::FS
