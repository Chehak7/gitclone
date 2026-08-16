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

class Repository {
public:
class Repository {
    bool init() {
        if (fs::exists(git_dir)) {
            return false;
        }

        fs::create_directory(git_dir);
        fs::create_directory(objects_dir);
        fs::create_directory(ref_dir);
        fs::create_directories(heads_dir);

        write_file_text(head_file, "ref: refs/heads/master\n");
        save_index({});

        std::cout << "Initialized empty Git repository in " << git_dir
                  << std::endl;
        return true;
    }
    std::string store_object(const GitObject& obj) {
        std::string obj_hash = obj.compute_hash();

        fs::path obj_dir = objects_dir / obj_hash.substr(0, 2);
        fs::path obj_file = obj_dir / obj_hash.substr(2);

        if (!fs::exists(obj_file)) {
            fs::create_directories(obj_dir);
            write_file_bytes(obj_file, obj.serialize());
        }

        return obj_hash;
    }
    GitObject load_object(const std::string& obj_hash) {
        fs::path obj_dir = objects_dir / obj_hash.substr(0, 2);
        fs::path obj_file = obj_dir / obj_hash.substr(2);

        if (!fs::exists(obj_file)) {
            throw std::runtime_error("Object " + obj_hash + " not found");
        }

        return GitObject::deserialize(read_file_bytes(obj_file));
    }
    std::map<std::string, std::string> load_index() {
        if (!fs::exists(index_file)) {
            return {};
        }
        try {
            std::string text = read_file_text(index_file);
            return index_deserialize(text);
        } catch (...) {
            return {};
        }
    }
    void save_index(const std::map<std::string, std::string>& index) {
        write_file_text(index_file, index_serialize(index));
    }
    void add_file(const std::string& path) {
        fs::path full_path = repo_path / path;
        if (!fs::exists(full_path)) {
            throw std::runtime_error("Path " + path + " not found");
        }

        std::vector<uint8_t> file_content = read_file_bytes(full_path);
        Blob blob(file_content);
        std::string blob_hash = store_object(blob);

        auto index = load_index();
        index[path] = blob_hash;
        save_index(index);

        std::cout << "Added " << path << std::endl;
    }
    void add_directory(const std::string& path) {
        fs::path full_path = repo_path / path;
        if (!fs::exists(full_path)) {
            throw std::runtime_error("Directory " + path + " not found");
        }
        if (!fs::is_directory(full_path)) {
            throw std::runtime_error(path + " is not a directory");
        }

        auto index = load_index();
        int added_count = 0;

        for (const auto& entry :
             fs::recursive_directory_iterator(full_path)) {
            if (!entry.is_regular_file()) continue;

            bool in_git = false;
            for (const auto& component : entry.path()) {
                if (component.string() == ".git") {
                    in_git = true;
                    break;
                }
            }
            if (in_git) continue;

            std::vector<uint8_t> file_content =
                read_file_bytes(entry.path());
            Blob blob(file_content);
            std::string blob_hash = store_object(blob);

            std::string rel_path =
                fs::relative(entry.path(), repo_path).string();
            index[rel_path] = blob_hash;
            ++added_count;
        }

        save_index(index);

        if (added_count > 0) {
            std::cout << "Added " << added_count << " files from directory "
                      << path << std::endl;
        } else {
            std::cout << "Directory " << path << " already up to date"
                      << std::endl;
        }
    }
    void add_path(const std::string& path) {
        fs::path full_path = repo_path / path;

        if (!fs::exists(full_path)) {
            throw std::runtime_error("Path " + path + " not found");
        }

        if (fs::is_regular_file(full_path)) {
            add_file(path);
        } else if (fs::is_directory(full_path)) {
            add_directory(path);
        } else {
            throw std::runtime_error(
                path + " is neither a file nor a directory");
        }
    }
    struct EntryValue {
        std::string hash;                              // non-empty = file
        std::map<std::string, EntryValue> children;    // non-empty = dir

        bool is_file() const { return !hash.empty(); }
    };
    std::string create_tree_from_index() {
        auto index = load_index();
        if (index.empty()) {
            Tree tree;
            return store_object(tree);
        }

        std::map<std::string, EntryValue> root;

        for (const auto& [file_path, blob_hash] : index) {
            auto parts = split(file_path, "/");

            if (parts.size() == 1) {
                root[parts[0]].hash = blob_hash;
            } else {
                auto* current = &root[parts[0]];
                for (size_t i = 1; i + 1 < parts.size(); ++i) {
                    current = &current->children[parts[i]];
                }
                current->children[parts.back()].hash = blob_hash;
            }
        }

        std::function<std::string(const std::map<std::string, EntryValue>&)>
            create_tree_recursive;
        create_tree_recursive =
            [&](const std::map<std::string, EntryValue>& entries)
                -> std::string {
            Tree tree;
            for (const auto& [name, entry] : entries) {
                if (entry.is_file()) {
                    tree.add_entry("100644", name, entry.hash);
                } else {
                    std::string subtree_hash =
                        create_tree_recursive(entry.children);
                    tree.add_entry("40000", name, subtree_hash);
                }
            }
            return store_object(tree);
        };

        return create_tree_recursive(root);
    }
                              "CppGit User <user@cppgit.com>") {
        std::string tree_hash = create_tree_from_index();

        std::string current_branch = get_current_branch();
        std::string parent_commit = get_branch_commit(current_branch);
        std::vector<std::string> parent_hashes;
        if (!parent_commit.empty()) {
            parent_hashes.push_back(parent_commit);
        }

        auto index = load_index();
        if (index.empty()) {
            std::cout << "nothing to commit, working tree clean" << std::endl;
            return "";
        }

        if (!parent_commit.empty()) {
            GitObject parent_obj = load_object(parent_commit);
            Commit parent_data = Commit::from_content(parent_obj.content);
            if (tree_hash == parent_data.tree_hash) {
                std::cout << "nothing to commit, working tree clean"
                          << std::endl;
                return "";
            }
        }

        Commit commit_obj(tree_hash, parent_hashes, author, author, message);
        std::string commit_hash = store_object(commit_obj);

        set_branch_commit(current_branch, commit_hash);

        // Keep this reset in mind: status and dirty checks fall back to the
        // committed tree when the index is empty.
        save_index({});

        std::cout << "Created commit " << commit_hash << " on branch "
                  << current_branch << std::endl;
        return commit_hash;
    }
        const std::string& tree_hash, const std::string& prefix = "") {
        std::set<std::string> files;
        try {
            GitObject tree_obj = load_object(tree_hash);
            Tree tree = Tree::from_content(tree_obj.content);

            for (const auto& entry : tree.entries) {
                std::string full_name = prefix + entry.name;
                if (entry.mode.size() >= 3 &&
                    entry.mode.substr(0, 3) == "100") {
                    files.insert(full_name);
                } else if (entry.mode.size() >= 3 &&
                           entry.mode.substr(0, 3) == "400") {
                    auto subtree_files = get_files_from_tree_recursive(
                        entry.obj_hash, full_name + "/");
                    files.insert(subtree_files.begin(), subtree_files.end());
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Warning: Could not read tree " << tree_hash
                      << ": " << e.what() << std::endl;
        }
        return files;
    }
        const std::string& tree_hash, const std::string& prefix = "") {
        std::map<std::string, std::string> index;
        try {
            GitObject tree_obj = load_object(tree_hash);
            Tree tree = Tree::from_content(tree_obj.content);

            for (const auto& entry : tree.entries) {
                std::string full_name = prefix + entry.name;
                if (entry.mode.size() >= 3 &&
                    entry.mode.substr(0, 3) == "100") {
                    index[full_name] = entry.obj_hash;
                } else if (entry.mode.size() >= 3 &&
                           entry.mode.substr(0, 3) == "400") {
                    auto subindex = build_index_from_tree(
                        entry.obj_hash, full_name + "/");
                    index.insert(subindex.begin(), subindex.end());
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Warning: Could not read tree " << tree_hash
                      << ": " << e.what() << std::endl;
        }
        return index;
    }
        const std::string& commit_hash) {
        if (commit_hash.empty()) return {};
        GitObject obj = load_object(commit_hash);
        Commit commit = Commit::from_content(obj.content);
        return commit.tree_hash.empty()
            ? std::map<std::string, std::string>{}
            : build_index_from_tree(commit.tree_hash);
    }
    std::vector<std::string> get_dirty_files() {
        std::string head = get_branch_commit(get_current_branch());
        auto committed_files = files_for_commit(head);
        auto index = load_index();
        std::vector<std::string> dirty_files;

        for (const auto& [fp, idx_hash] : index) {
            auto it = committed_files.find(fp);
            if (it == committed_files.end() || it->second != idx_hash) {
                dirty_files.push_back(fp);
            }
        }

        auto expected_files = committed_files;
        for (const auto& [fp, idx_hash] : index) {
            expected_files[fp] = idx_hash;
        }
        for (const auto& [fp, expected_hash] : expected_files) {
            fs::path full = repo_path / fp;
            if (!fs::is_regular_file(full)) {
                dirty_files.push_back(fp);
                continue;
            }
            Blob blob(read_file_bytes(full));
            if (blob.compute_hash() != expected_hash) {
                dirty_files.push_back(fp);
            }
        }

        std::sort(dirty_files.begin(), dirty_files.end());
        dirty_files.erase(std::unique(dirty_files.begin(), dirty_files.end()),
                          dirty_files.end());
        return dirty_files;
    }
    void restore_tree(const std::string& tree_hash, const fs::path& path) {
        GitObject tree_obj = load_object(tree_hash);
        Tree tree = Tree::from_content(tree_obj.content);

        for (const auto& entry : tree.entries) {
            fs::path file_path = path / entry.name;

            if (entry.mode.size() >= 3 &&
                entry.mode.substr(0, 3) == "100") {
                GitObject blob_obj = load_object(entry.obj_hash);
                write_file_bytes(file_path, blob_obj.content);
            } else if (entry.mode.size() >= 3 &&
                       entry.mode.substr(0, 3) == "400") {
                fs::create_directories(file_path);
                restore_tree(entry.obj_hash, file_path);
            }
        }
    }
                                   const std::set<std::string>& files_to_clear) {
        std::string target_commit_hash = get_branch_commit(branch);
        if (target_commit_hash.empty()) return;

        for (const auto& rel_path : files_to_clear) {
            fs::path file_path = repo_path / rel_path;
            try {
                if (fs::is_regular_file(file_path)) {
                    fs::remove(file_path);
                }
            } catch (...) {}
        }

        GitObject target_obj = load_object(target_commit_hash);
        Commit target_commit = Commit::from_content(target_obj.content);

        if (!target_commit.tree_hash.empty()) {
            restore_tree(target_commit.tree_hash, repo_path);
        }

        save_index({});
    }
                                     const std::string& target_hash) {
        std::set<std::string> current_ancestors;
        std::vector<std::string> pending{current_hash};
        for (size_t i = 0; i < pending.size(); ++i) {
            const std::string hash = pending[i];
            if (hash.empty() || !current_ancestors.insert(hash).second) continue;
            Commit commit = Commit::from_content(load_object(hash).content);
            pending.insert(pending.end(), commit.parent_hashes.begin(),
                           commit.parent_hashes.end());
        }

        std::set<std::string> seen;
        pending = {target_hash};
        for (size_t i = 0; i < pending.size(); ++i) {
            const std::string hash = pending[i];
            if (hash.empty() || !seen.insert(hash).second) continue;
            if (current_ancestors.count(hash) != 0) return hash;
            Commit commit = Commit::from_content(load_object(hash).content);
            pending.insert(pending.end(), commit.parent_hashes.begin(),
                           commit.parent_hashes.end());
        }
        return "";
    }
        const std::set<std::string>& files_to_clear) {
        for (const auto& fp : files_to_clear) {
            fs::path path = repo_path / fp;
            if (fs::is_regular_file(path)) fs::remove(path);
        }
        for (const auto& [fp, hash] : files) {
            fs::path path = repo_path / fp;
            if (!path.parent_path().empty()) {
                fs::create_directories(path.parent_path());
            }
            write_file_bytes(path, load_object(hash).content);
        }
    }
    bool merge(const std::string& target_branch) {
        std::string current_branch = get_current_branch();
        std::string current_hash = get_branch_commit(current_branch);
        std::string target_hash = get_branch_commit(target_branch);
        if (target_hash.empty()) {
            std::cerr << "merge: " << target_branch << " - not something we can merge"
                      << std::endl;
            return false;
        }
        if (current_hash.empty()) {
            std::cerr << "merge: current branch has no commits" << std::endl;
            return false;
        }
        if (!require_clean_working_tree("merge")) return false;

        std::string ancestor_hash = find_common_ancestor(current_hash, target_hash);
        if (ancestor_hash.empty()) {
            std::cerr << "fatal: refusing to merge unrelated histories"
                      << std::endl;
            return false;
        }
        if (target_hash == ancestor_hash) {
            std::cout << "Already up to date." << std::endl;
            return true;
        }

        auto current_files = files_for_commit(current_hash);
        auto target_files = files_for_commit(target_hash);
        std::set<std::string> files_to_clear;
        for (const auto& [fp, hash] : current_files) {
            (void)hash;
            files_to_clear.insert(fp);
        }
        for (const auto& [fp, hash] : target_files) {
            (void)hash;
            files_to_clear.insert(fp);
        }

        if (current_hash == ancestor_hash) {
            write_working_files(target_files, files_to_clear);
            set_branch_commit(current_branch, target_hash);
            save_index({});
            std::cout << "Fast-forward" << std::endl;
            return true;
        }

        auto ancestor_files = files_for_commit(ancestor_hash);
        std::set<std::string> all_paths;
        for (const auto& [fp, hash] : ancestor_files) {
            (void)hash;
            all_paths.insert(fp);
        }
        for (const auto& [fp, hash] : current_files) {
            (void)hash;
            all_paths.insert(fp);
        }
        for (const auto& [fp, hash] : target_files) {
            (void)hash;
            all_paths.insert(fp);
        }

        auto hash_at = [](const std::map<std::string, std::string>& files,
                          const std::string& path) {
            auto it = files.find(path);
            return it == files.end() ? std::string{} : it->second;
        };
        std::map<std::string, std::string> merged_files;
        std::vector<std::string> conflicts;
        for (const auto& fp : all_paths) {
            std::string ancestor = hash_at(ancestor_files, fp);
            std::string current = hash_at(current_files, fp);
            std::string target = hash_at(target_files, fp);
            std::string result;

            if (current == target) {
                result = current;
            } else if (current == ancestor) {
                result = target;
            } else if (target == ancestor) {
                result = current;
            } else {
                conflicts.push_back(fp);
                continue;
            }
            if (!result.empty()) merged_files[fp] = result;
        }

        write_working_files(merged_files, files_to_clear);
        if (!conflicts.empty()) {
            for (const auto& fp : conflicts) {
                std::string current_hash_for_file = hash_at(current_files, fp);
                std::string target_hash_for_file = hash_at(target_files, fp);
                std::string current_text;
                std::string target_text;
                if (!current_hash_for_file.empty()) {
                    auto bytes = load_object(current_hash_for_file).content;
                    current_text.assign(bytes.begin(), bytes.end());
                }
                if (!target_hash_for_file.empty()) {
                    auto bytes = load_object(target_hash_for_file).content;
                    target_text.assign(bytes.begin(), bytes.end());
                }
                fs::path path = repo_path / fp;
                fs::create_directories(path.parent_path());
                std::string contents = "<<<<<<< HEAD\n" + current_text;
                if (!current_text.empty() && current_text.back() != '\n') contents += "\n";
                contents += "=======\n" + target_text;
                if (!target_text.empty() && target_text.back() != '\n') contents += "\n";
                contents += ">>>>>>> " + target_branch + "\n";
                write_file_text(path, contents);
            }
            save_index({});
            std::cout << "CONFLICT (content): Merge conflict in:" << std::endl;
            for (const auto& fp : conflicts) std::cout << "    " << fp << std::endl;
            std::cout << "Automatic merge failed; fix conflicts and then commit the result."
                      << std::endl;
            return false;
        }

        save_index(merged_files);
        std::string tree_hash = create_tree_from_index();
        std::string author = "CppGit User <user@cppgit.com>";
        Commit commit(tree_hash, {current_hash, target_hash}, author, author,
                      "Merge branch '" + target_branch + "'");
        std::string merge_hash = store_object(commit);
        set_branch_commit(current_branch, merge_hash);
        save_index({});
        std::cout << "Merge made commit " << merge_hash << std::endl;
        return true;
    }
    void log(int max_count = 10) {
        std::string current_branch = get_current_branch();
        std::string commit_hash = get_branch_commit(current_branch);

        if (commit_hash.empty()) {
            std::cout << "No commits yet!" << std::endl;
            return;
        }

        int count = 0;
        while (!commit_hash.empty() && count < max_count) {
            GitObject commit_obj = load_object(commit_hash);
            Commit c = Commit::from_content(commit_obj.content);

            std::cout << "commit " << commit_hash << std::endl;
            std::cout << "Author: " << c.author << std::endl;

            std::time_t t = static_cast<std::time_t>(c.timestamp);
            std::cout << "Date: " << std::ctime(&t);  // ctime adds \n
            std::cout << "\n    " << c.message << "\n" << std::endl;

            commit_hash = c.parent_hashes.empty() ? "" : c.parent_hashes[0];
            ++count;
        }
    }
    void status() {
        std::string current_branch = get_current_branch();
        std::cout << "On branch " << current_branch << std::endl;

        auto index = load_index();
        std::string current_commit_hash = get_branch_commit(current_branch);

        std::map<std::string, std::string> last_index_files;
        if (!current_commit_hash.empty()) {
            try {
                GitObject commit_obj = load_object(current_commit_hash);
                Commit c = Commit::from_content(commit_obj.content);
                if (!c.tree_hash.empty()) {
                    last_index_files = build_index_from_tree(c.tree_hash);
                }
            } catch (...) {
                last_index_files.clear();
            }
        }

        std::map<std::string, std::string> working_files;
        for (const auto& file : get_all_files()) {
            std::string rel_path =
                fs::relative(file, repo_path).string();
            try {
                auto file_content = read_file_bytes(file);
                Blob blob(file_content);
                working_files[rel_path] = blob.compute_hash();
            } catch (...) {
                continue;
            }
        }

        std::vector<std::pair<std::string, std::string>> staged_files;

        std::set<std::string> all_index_paths;
        for (const auto& [k, v] : index) {
            (void)v;
            all_index_paths.insert(k);
        }
        for (const auto& [k, v] : last_index_files) {
            (void)v;
            all_index_paths.insert(k);
        }

        for (const auto& file_path : all_index_paths) {
            auto idx_it = index.find(file_path);
            auto last_it = last_index_files.find(file_path);

            std::string index_hash =
                (idx_it != index.end()) ? idx_it->second : "";
            std::string last_hash =
                (last_it != last_index_files.end()) ? last_it->second : "";

            if (!index_hash.empty() && last_hash.empty()) {
                staged_files.push_back({"new file", file_path});
            } else if (!index_hash.empty() && !last_hash.empty() &&
                       index_hash != last_hash) {
                staged_files.push_back({"modified", file_path});
            }
        }

        if (!staged_files.empty()) {
            std::sort(staged_files.begin(), staged_files.end());
            std::cout << "\nChanges to be committed:" << std::endl;
            for (const auto& [stage_status, fp] : staged_files) {
                std::cout << "   " << stage_status << ": " << fp << std::endl;
            }
        }

        // A staged entry overrides the committed baseline for that path.
        auto expected_files = last_index_files;
        for (const auto& [fp, idx_hash] : index) {
            expected_files[fp] = idx_hash;
        }

        std::vector<std::string> unstaged_files;
        for (const auto& [fp, wf_hash] : working_files) {
            auto expected_it = expected_files.find(fp);
            if (expected_it != expected_files.end() &&
                wf_hash != expected_it->second) {
                unstaged_files.push_back(fp);
            }
        }

        if (!unstaged_files.empty()) {
            std::sort(unstaged_files.begin(), unstaged_files.end());
            std::cout << "\nChanges not staged for commit:" << std::endl;
            for (const auto& fp : unstaged_files) {
                std::cout << "   modified: " << fp << std::endl;
            }
        }

        std::vector<std::string> untracked_files;
        for (const auto& [fp, wf_hash] : working_files) {
            (void)wf_hash;
            if (index.find(fp) == index.end() &&
                last_index_files.find(fp) == last_index_files.end()) {
                untracked_files.push_back(fp);
            }
        }

        if (!untracked_files.empty()) {
            std::sort(untracked_files.begin(), untracked_files.end());
            std::cout << "\nUntracked files:" << std::endl;
            for (const auto& fp : untracked_files) {
                std::cout << "   " << fp << std::endl;
            }
        }

        std::vector<std::string> deleted_files;
        for (const auto& [fp, expected_hash] : expected_files) {
            (void)expected_hash;
            if (working_files.find(fp) == working_files.end()) {
                deleted_files.push_back(fp);
            }
        }

        if (!deleted_files.empty()) {
            std::sort(deleted_files.begin(), deleted_files.end());
            std::cout << "\nDeleted files:" << std::endl;
            for (const auto& fp : deleted_files) {
                std::cout << "   deleted: " << fp << std::endl;
            }
        }

        if (staged_files.empty() && unstaged_files.empty() &&
            deleted_files.empty() && untracked_files.empty()) {
            std::cout << "\nnothing to commit, working tree clean"
                      << std::endl;
        }
    }
};

int main(int argc, char* argv[]) {
    return 0;
}
