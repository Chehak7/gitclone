// main.cpp — Minimal git implementation in C++17.
// Ported from github.com/RivaanRanawat/git-clone-python
//
// Uses only macOS system libs: CommonCrypto (SHA-1), zlib, <filesystem>.
// Build: g++ -std=c++17 -Wall -Wextra -O2 -o mygit main.cpp -lz

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>       // for recursive lambdas in create_tree_from_index
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include <CommonCrypto/CommonDigest.h>  // CC_SHA1
#include <zlib.h>

namespace fs = std::filesystem;

// Utilities
static std::string bytes_to_hex(const std::vector<uint8_t>& bytes) {
    std::ostringstream oss;
    for (uint8_t b : bytes) {
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(b);
    }
    return oss.str();
}

static std::string bytes_to_hex(const uint8_t* data, size_t len) {
    std::ostringstream oss;
    for (size_t i = 0; i < len; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(data[i]);
    }
    return oss.str();
}

static std::vector<uint8_t> hex_to_bytes(const std::string& hex) {
    std::vector<uint8_t> result;
    result.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        uint8_t byte = static_cast<uint8_t>(
            std::stoi(hex.substr(i, 2), nullptr, 16));
        result.push_back(byte);
    }
    return result;
}

// CommonCrypto keeps this build independent of OpenSSL.
static std::string sha1_hex(const uint8_t* data, size_t len) {
    uint8_t digest[CC_SHA1_DIGEST_LENGTH];
    CC_SHA1(data, static_cast<CC_LONG>(len), digest);
    return bytes_to_hex(digest, CC_SHA1_DIGEST_LENGTH);
}

static std::string sha1_hex(const std::vector<uint8_t>& data) {
    return sha1_hex(data.data(), data.size());
}

static std::vector<uint8_t> read_file_bytes(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Cannot open file: " + path.string());
    }
    return std::vector<uint8_t>(
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
}

static void write_file_bytes(const fs::path& path,
                             const std::vector<uint8_t>& data) {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Cannot write file: " + path.string());
    }
    file.write(reinterpret_cast<const char*>(data.data()),
               static_cast<std::streamsize>(data.size()));
}

static std::string read_file_text(const fs::path& path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("Cannot open file: " + path.string());
    }
    std::ostringstream oss;
    oss << file.rdbuf();
    return oss.str();
}

static void write_file_text(const fs::path& path, const std::string& text) {
    std::ofstream file(path);
    if (!file) {
        throw std::runtime_error("Cannot write file: " + path.string());
    }
    file << text;
}

// HEAD and ref files normally have a trailing newline.
static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\n\r");
    return s.substr(start, end - start + 1);
}

// With a limit, leave the unsplit remainder in the last element.
static std::vector<std::string> split(const std::string& s,
                                      const std::string& delim,
                                      int max_splits = -1) {
    std::vector<std::string> result;
    size_t start = 0;
    int splits = 0;
    while (start < s.size()) {
        if (max_splits >= 0 && splits >= max_splits) {
            result.push_back(s.substr(start));
            break;
        }
        size_t pos = s.find(delim, start);
        if (pos == std::string::npos) {
            result.push_back(s.substr(start));
            break;
        }
        result.push_back(s.substr(start, pos - start));
        start = pos + delim.size();
        ++splits;
    }
    return result;
}

// Commit identities contain spaces, so their timestamp fields are easier to
// peel off from the right.
static std::vector<std::string> rsplit(const std::string& s,
                                       const std::string& delim,
                                       int max_splits = -1) {
    if (max_splits < 0) return split(s, delim);
    std::vector<size_t> positions;
    size_t pos = 0;
    while ((pos = s.find(delim, pos)) != std::string::npos) {
        positions.push_back(pos);
        pos += delim.size();
    }
    int skip = static_cast<int>(positions.size()) - max_splits;
    if (skip < 0) skip = 0;

    std::vector<std::string> result;
    size_t start = 0;
    for (size_t i = static_cast<size_t>(skip); i < positions.size(); ++i) {
        result.push_back(s.substr(start, positions[i] - start));
        start = positions[i] + delim.size();
    }
    result.push_back(s.substr(start));
    return result;
}

// One "path\thash\n" per entry. The old hand-rolled JSON broke on quotes and
// backslashes in filenames; this is simpler than dragging in a JSON library.
static std::string index_serialize(const std::map<std::string, std::string>& m) {
    std::ostringstream oss;
    for (const auto& [key, value] : m) {
        oss << key << "\t" << value << "\n";
    }
    return oss.str();
}

static std::map<std::string, std::string> index_deserialize(
    const std::string& text) {
    std::map<std::string, std::string> result;
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.empty()) continue;
        size_t tab_pos = line.find('\t');
        if (tab_pos == std::string::npos) continue;  // malformed line, skip
        std::string key = line.substr(0, tab_pos);
        std::string value = line.substr(tab_pos + 1);
        if (!key.empty() && !value.empty()) {
            result[key] = value;
        }
    }
    return result;
}

static std::vector<uint8_t> zlib_compress(const std::vector<uint8_t>& data) {
    uLongf compressed_size = compressBound(static_cast<uLong>(data.size()));
    std::vector<uint8_t> result(compressed_size);

    int ret = compress(result.data(), &compressed_size,
                       data.data(), static_cast<uLong>(data.size()));
    if (ret != Z_OK) {
        throw std::runtime_error("zlib compress failed with code " +
                                 std::to_string(ret));
    }
    result.resize(compressed_size);
    return result;
}

static std::vector<uint8_t> zlib_decompress(const std::vector<uint8_t>& data) {
    // Keep capacity separate: uncompress() may rewrite dest_len on
    // Z_BUF_ERROR. Reusing it here made retries stop growing correctly.
    size_t buf_capacity = data.size() * 4;
    if (buf_capacity < 128) buf_capacity = 128;
    std::vector<uint8_t> result;

    for (int attempt = 0; attempt < 10; ++attempt) {
        result.resize(buf_capacity);
        uLongf dest_len = static_cast<uLongf>(buf_capacity);
        int ret = uncompress(result.data(), &dest_len,
                             data.data(), static_cast<uLong>(data.size()));
        if (ret == Z_OK) {
            result.resize(dest_len);
            return result;
        }
        if (ret == Z_BUF_ERROR) {
            buf_capacity *= 2;
            continue;
        }
        throw std::runtime_error("zlib decompress failed with code " +
                                 std::to_string(ret));
    }
    throw std::runtime_error("zlib decompress: buffer too small after retries");
}

// Git objects

// On disk these are zlib-compressed "<type> <content-length>\0<content>".
// The object ID is the SHA-1 of that whole uncompressed byte sequence.
class GitObject {
public:
    std::string type;               // "blob", "tree", or "commit"
    std::vector<uint8_t> content;

    GitObject(const std::string& obj_type, const std::vector<uint8_t>& content)
        : type(obj_type), content(content) {}

    virtual ~GitObject() = default;

    std::string compute_hash() const {
        std::string header = type + " " + std::to_string(content.size());
        header.push_back('\0');

        std::vector<uint8_t> full_data(header.begin(), header.end());
        full_data.insert(full_data.end(), content.begin(), content.end());

        return sha1_hex(full_data);
    }

    std::vector<uint8_t> serialize() const {
        std::string header = type + " " + std::to_string(content.size());
        header.push_back('\0');

        std::vector<uint8_t> full_data(header.begin(), header.end());
        full_data.insert(full_data.end(), content.begin(), content.end());

        return zlib_compress(full_data);
    }

    static GitObject deserialize(const std::vector<uint8_t>& data) {
        std::vector<uint8_t> decompressed = zlib_decompress(data);

        // The NUL is the actual format boundary, not whitespace.
        auto null_it = std::find(decompressed.begin(), decompressed.end(), '\0');
        if (null_it == decompressed.end()) {
            throw std::runtime_error("Invalid git object: no null separator");
        }

        std::string header(decompressed.begin(), null_it);
        std::vector<uint8_t> obj_content(null_it + 1, decompressed.end());

        auto parts = split(header, " ");
        if (parts.empty()) {
            throw std::runtime_error("Invalid git object header");
        }

        return GitObject(parts[0], obj_content);
    }
};

class Blob : public GitObject {
public:
    explicit Blob(const std::vector<uint8_t>& content)
        : GitObject("blob", content) {}
};

// Tree entries are concatenated with no delimiter between entries:
//   "<mode> <name>\0<20-byte-binary-hash>"
// Sorting by mode/name is deterministic but differs from Git, which compares
// names and treats directories as if they had a trailing '/'.
struct TreeEntry {
    std::string mode;       // "100644" (file) or "40000" (dir)
    std::string name;
    std::string obj_hash;   // 40-char hex

    bool operator<(const TreeEntry& other) const {
        if (mode != other.mode) return mode < other.mode;
        return name < other.name;
    }
};

class Tree : public GitObject {
public:
    std::vector<TreeEntry> entries;

    explicit Tree(const std::vector<TreeEntry>& entries = {})
        : GitObject("tree", {}), entries(entries) {
        content = serialize_entries();
    }

    // This is the raw 20-byte hash, not its 40-character hex spelling. Easy
    // detail to miss when looking at loose objects by hand.
    std::vector<uint8_t> serialize_entries() const {
        std::vector<TreeEntry> sorted_entries = entries;
        std::sort(sorted_entries.begin(), sorted_entries.end());

        std::vector<uint8_t> result;
        for (const auto& entry : sorted_entries) {
            std::string mode_name = entry.mode + " " + entry.name;
            result.insert(result.end(), mode_name.begin(), mode_name.end());
            result.push_back('\0');

            std::vector<uint8_t> hash_bytes = hex_to_bytes(entry.obj_hash);
            result.insert(result.end(), hash_bytes.begin(), hash_bytes.end());
        }
        return result;
    }

    void add_entry(const std::string& mode, const std::string& name,
                   const std::string& obj_hash) {
        entries.push_back({mode, name, obj_hash});
        content = serialize_entries();
    }

    // There is no entry count, so reaching the end of the object ends the tree.
    static Tree from_content(const std::vector<uint8_t>& data) {
        Tree tree;
        size_t i = 0;

        while (i < data.size()) {
            auto null_it = std::find(data.begin() + static_cast<long>(i),
                                     data.end(), '\0');
            if (null_it == data.end()) break;

            size_t null_idx = static_cast<size_t>(null_it - data.begin());

            std::string mode_name(data.begin() + static_cast<long>(i),
                                  data.begin() + static_cast<long>(null_idx));

            auto parts = split(mode_name, " ", 1);
            if (parts.size() < 2) break;

            std::string mode = parts[0];
            std::string name = parts[1];

            size_t hash_start = null_idx + 1;
            if (hash_start + 20 > data.size()) break;

            std::vector<uint8_t> hash_bytes(data.begin() + static_cast<long>(hash_start),
                                            data.begin() + static_cast<long>(hash_start + 20));
            std::string obj_hash = bytes_to_hex(hash_bytes);

            tree.entries.push_back({mode, name, obj_hash});

            i = hash_start + 20;
        }
        return tree;
    }
};

// Commit objects use this plain-text header:
//   tree <tree_hash>
//   parent <parent_hash>       ← zero or more
//   author <name> <timestamp> +0000
//   committer <name> <timestamp> +0000
//
//   <message>
class Commit : public GitObject {
public:
    std::string tree_hash;
    std::vector<std::string> parent_hashes;
    std::string author;
    std::string committer;
    std::string message;
    int64_t timestamp;

    Commit(const std::string& tree_hash,
           const std::vector<std::string>& parent_hashes,
           const std::string& author,
           const std::string& committer,
           const std::string& message,
           int64_t timestamp = 0)
        : GitObject("commit", {}),
          tree_hash(tree_hash),
          parent_hashes(parent_hashes),
          author(author),
          committer(committer),
          message(message),
          timestamp(timestamp == 0 ? static_cast<int64_t>(std::time(nullptr))
                                   : timestamp) {
        content = serialize_commit();
    }

    std::vector<uint8_t> serialize_commit() const {
        std::ostringstream oss;
        oss << "tree " << tree_hash << "\n";

        for (const auto& parent : parent_hashes) {
            oss << "parent " << parent << "\n";
        }

        oss << "author " << author << " " << timestamp << " +0000\n";
        oss << "committer " << committer << " " << timestamp << " +0000\n";
        oss << "\n";
        oss << message;

        std::string s = oss.str();
        return std::vector<uint8_t>(s.begin(), s.end());
    }

    static Commit from_content(const std::vector<uint8_t>& data) {
        std::string text(data.begin(), data.end());
        auto lines = split(text, "\n");

        std::string t_hash;
        std::vector<std::string> p_hashes;
        std::string auth;
        std::string comm;
        int64_t ts = 0;
        size_t message_start = 0;

        for (size_t i = 0; i < lines.size(); ++i) {
            const auto& line = lines[i];

            if (line.size() >= 5 && line.substr(0, 5) == "tree ") {
                t_hash = line.substr(5);
            } else if (line.size() >= 7 && line.substr(0, 7) == "parent ") {
                p_hashes.push_back(line.substr(7));
            } else if (line.size() >= 7 && line.substr(0, 7) == "author ") {
                // Work from the right because names and emails may contain spaces.
                auto parts = rsplit(line.substr(7), " ", 2);
                if (parts.size() >= 2) {
                    auth = parts[0];
                    try { ts = std::stoll(parts[1]); } catch (...) {}
                }
            } else if (line.size() >= 10 && line.substr(0, 10) == "committer ") {
                auto parts = rsplit(line.substr(10), " ", 2);
                if (!parts.empty()) {
                    comm = parts[0];
                }
            } else if (line.empty()) {
                message_start = i + 1;
                break;
            }
        }

        std::string msg;
        for (size_t i = message_start; i < lines.size(); ++i) {
            if (i > message_start) msg += "\n";
            msg += lines[i];
        }

        return Commit(t_hash, p_hashes, auth, comm, msg, ts);
    }
};

// Repository

// .git/ layout:
//   .git/
//   ├── HEAD            ← "ref: refs/heads/master\n"
//   ├── index           ← tab-delimited file: filepath → blob hash
//   ├── objects/         ← compressed git objects as objects/XX/YYYY...
//   └── refs/
//       └── heads/      ← one file per branch containing a commit hash
//           └── master  ← e.g., "abc123...\n"

int main(int argc, char* argv[]) {
    return 0;
}
