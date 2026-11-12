#include "disk_manager.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace {

std::string CanonicalizePath(const std::string &path)
{
    std::error_code ec;
    std::filesystem::path abs_path = std::filesystem::absolute(std::filesystem::path(path), ec);
    if (ec)
    {
        return path;
    }

    std::filesystem::path canon = std::filesystem::weakly_canonical(abs_path, ec);
    if (ec)
    {
        return abs_path.lexically_normal().string();
    }

    return canon.lexically_normal().string();
}

}  // namespace

struct DiskManager::FileState {
    std::mutex mutex;
    page_id_t next_page_id{0};
    bool initialized{false};
};

// File-path-keyed state registry.
// All DiskManager instances that open the same canonical file path share one FileState,
// so their I/O sequences and page allocation cursors stay synchronized.
// Different files remain completely independent.
std::shared_ptr<DiskManager::FileState> DiskManager::GetFileState(const std::string &file_name)
{
    static std::mutex registry_mutex;
    static std::unordered_map<std::string, std::weak_ptr<FileState>> file_states;

    const std::string key = CanonicalizePath(file_name);

    std::lock_guard<std::mutex> reg_lock(registry_mutex);
    for (auto it = file_states.begin(); it != file_states.end(); )
    {
        if (it->second.expired())
        {
            it = file_states.erase(it);
        }
        else
        {
            ++it;
        }
    }

    auto &weak = file_states[key];
    auto shared = weak.lock();
    if (!shared)
    {
        shared = std::make_shared<FileState>();
        weak = shared;
    }
    return shared;
}

DiskManager::DiskManager(const std::string &file_name)
    : file_name_(file_name),
      file_state_(GetFileState(file_name))
{
    std::lock_guard<std::mutex> lock(file_state_->mutex);

    db_file_.open(file_name,
                  std::ios::binary |
                  std::ios::in |
                  std::ios::out);

    if (!db_file_.is_open())
    {
        std::ofstream create(file_name, std::ios::binary);

        if (!create.is_open())
        {
            throw std::runtime_error("Failed to create database file: " + file_name);
        }

        create.close();

        db_file_.open(file_name,
                      std::ios::binary |
                      std::ios::in |
                      std::ios::out);

        if (!db_file_.is_open())
        {
            throw std::runtime_error("Failed to open database file: " + file_name);
        }
    }

    db_file_.seekg(0, std::ios::end);

    std::streamoff size = db_file_.tellg();

    if (size == -1)
    {
        throw std::runtime_error("Failed to determine database file size.");
    }

    if (size % PAGE_SIZE != 0)
    {
        throw std::runtime_error("Database file is corrupted.");
    }

    if (!file_state_->initialized)
    {
        file_state_->next_page_id = static_cast<page_id_t>(size / PAGE_SIZE);
        file_state_->initialized = true;
    }
}

page_id_t DiskManager::AllocatePage()
{
    std::lock_guard<std::mutex> lock(file_state_->mutex);
    return file_state_->next_page_id++;
}

void DiskManager::DeallocatePage(page_id_t page_id)
{
    // Logical deallocation placeholder; reserved for future free-list/bitmap reclaiming.
    (void)page_id;
}

page_id_t DiskManager::GetNextPageId() const
{
    std::lock_guard<std::mutex> lock(file_state_->mutex);
    return file_state_->next_page_id;
}

void DiskManager::WritePage(page_id_t page_id, const char *data)
{
    if (page_id < 0)
    {
        throw std::invalid_argument("WritePage: invalid negative page_id.");
    }
    if (data == nullptr)
    {
        throw std::invalid_argument("WritePage: data buffer cannot be nullptr.");
    }

    std::lock_guard<std::mutex> lock(file_state_->mutex);

    const size_t offset = static_cast<size_t>(page_id) * PAGE_SIZE;

    // Use the write pointer to determine current file size.
    // Using seekp/tellp keeps read and write positions conceptually separate.
    db_file_.seekp(0, std::ios::end);

    if (!db_file_)
    {
        throw std::runtime_error("Failed to seek to end of file.");
    }

    std::streamoff file_size = db_file_.tellp();

    if (file_size == -1)
    {
        throw std::runtime_error("Failed to determine file size.");
    }

    if (file_size % PAGE_SIZE != 0)
    {
        throw std::runtime_error("Corrupted database file.");
    }

    char zero_page[PAGE_SIZE] = {};

    while (static_cast<size_t>(file_size) < offset)
    {
        db_file_.write(zero_page, PAGE_SIZE);

        if (!db_file_)
        {
            throw std::runtime_error("Failed while zero-filling pages.");
        }

        file_size += PAGE_SIZE;
    }

    db_file_.seekp(offset);

    if (!db_file_)
    {
        throw std::runtime_error("Failed to seek page.");
    }

    db_file_.write(data, PAGE_SIZE);

    if (!db_file_)
    {
        throw std::runtime_error("Failed to write page.");
    }

    db_file_.flush();

    if (!db_file_)
    {
        throw std::runtime_error("Failed to flush file.");
    }

    num_writes_.fetch_add(1, std::memory_order_relaxed);
}

void DiskManager::ReadPage(page_id_t page_id, char *buffer)
{
    if (page_id < 0)
    {
        throw std::invalid_argument("ReadPage: invalid negative page_id.");
    }
    if (buffer == nullptr)
    {
        throw std::invalid_argument("ReadPage: destination buffer cannot be nullptr.");
    }
    if (static_cast<uint64_t>(page_id) > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()) / PAGE_SIZE ||
        static_cast<size_t>(page_id) > std::numeric_limits<size_t>::max() / PAGE_SIZE)
    {
        throw std::overflow_error("ReadPage: page offset overflows representable file offset.");
    }

    const std::streamoff offset = static_cast<std::streamoff>(static_cast<size_t>(page_id) * PAGE_SIZE);

    std::lock_guard<std::mutex> guard(file_state_->mutex);

    std::memset(buffer, 0, PAGE_SIZE);

    db_file_.seekg(offset);

    if (!db_file_)
    {
        throw std::runtime_error("Failed to seek.");
    }

    db_file_.read(buffer, PAGE_SIZE);

    if (db_file_.bad())
    {
        throw std::runtime_error("I/O error while reading page.");
    }

    std::streamsize bytes_read = db_file_.gcount();

    if (bytes_read < static_cast<std::streamsize>(PAGE_SIZE))
    {
        db_file_.clear();
    }

    num_reads_.fetch_add(1, std::memory_order_relaxed);
}

DiskManager::~DiskManager()
{
    if (file_state_)
    {
        std::lock_guard<std::mutex> guard(file_state_->mutex);
        if (db_file_.is_open())
        {
            db_file_.close();
        }
    }
}
