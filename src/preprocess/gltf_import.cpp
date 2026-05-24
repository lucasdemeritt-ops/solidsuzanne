// VGEO glTF 2.0 Importer
// Full glTF 2.0 parser supporting .gltf (JSON + .bin) and .glb (binary) files

#include "gltf_import.h"

#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <unordered_map>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <memory>

namespace vgeo {

// ============================================================================
// Minimal JSON Parser
// ============================================================================

enum class JsonType {
    Null,
    Bool,
    Number,
    String,
    Array,
    Object
};

struct JsonValue;
using JsonObject = std::unordered_map<std::string, std::shared_ptr<JsonValue>>;
using JsonArray = std::vector<std::shared_ptr<JsonValue>>;

struct JsonValue {
    JsonType type = JsonType::Null;
    bool bool_value = false;
    double number_value = 0.0;
    std::string string_value;
    JsonArray array_value;
    JsonObject object_value;

    bool is_null() const { return type == JsonType::Null; }
    bool is_bool() const { return type == JsonType::Bool; }
    bool is_number() const { return type == JsonType::Number; }
    bool is_string() const { return type == JsonType::String; }
    bool is_array() const { return type == JsonType::Array; }
    bool is_object() const { return type == JsonType::Object; }

    bool as_bool(bool default_val = false) const {
        return is_bool() ? bool_value : default_val;
    }

    double as_number(double default_val = 0.0) const {
        return is_number() ? number_value : default_val;
    }

    int as_int(int default_val = 0) const {
        return is_number() ? static_cast<int>(number_value) : default_val;
    }

    const std::string& as_string() const {
        static std::string empty;
        return is_string() ? string_value : empty;
    }

    const JsonArray& as_array() const {
        static JsonArray empty;
        return is_array() ? array_value : empty;
    }

    const JsonObject& as_object() const {
        static JsonObject empty;
        return is_object() ? object_value : empty;
    }

    bool has(const std::string& key) const {
        if (!is_object()) return false;
        return object_value.find(key) != object_value.end();
    }

    const JsonValue& operator[](const std::string& key) const {
        static JsonValue null_value;
        if (!is_object()) return null_value;
        auto it = object_value.find(key);
        if (it == object_value.end()) return null_value;
        return *it->second;
    }

    const JsonValue& operator[](size_t index) const {
        static JsonValue null_value;
        if (!is_array() || index >= array_value.size()) return null_value;
        return *array_value[index];
    }

    size_t size() const {
        if (is_array()) return array_value.size();
        if (is_object()) return object_value.size();
        return 0;
    }
};

class JsonParser {
public:
    std::shared_ptr<JsonValue> parse(const std::string& json) {
        pos_ = 0;
        json_ = &json;
        return parse_value();
    }

private:
    const std::string* json_ = nullptr;
    size_t pos_ = 0;

    char peek() const {
        return pos_ < json_->size() ? (*json_)[pos_] : '\0';
    }

    char get() {
        return pos_ < json_->size() ? (*json_)[pos_++] : '\0';
    }

    void skip_whitespace() {
        while (pos_ < json_->size()) {
            char c = (*json_)[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                pos_++;
            } else {
                break;
            }
        }
    }

    std::shared_ptr<JsonValue> parse_value() {
        skip_whitespace();
        char c = peek();

        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (c == '"') return parse_string();
        if (c == 't' || c == 'f') return parse_bool();
        if (c == 'n') return parse_null();
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number();

        return std::make_shared<JsonValue>();
    }

    std::shared_ptr<JsonValue> parse_object() {
        auto value = std::make_shared<JsonValue>();
        value->type = JsonType::Object;

        get(); // consume '{'
        skip_whitespace();

        if (peek() == '}') {
            get();
            return value;
        }

        while (true) {
            skip_whitespace();

            // Parse key
            if (peek() != '"') return value;
            auto key_value = parse_string();
            std::string key = key_value->string_value;

            skip_whitespace();
            if (get() != ':') return value;

            // Parse value
            auto val = parse_value();
            value->object_value[key] = val;

            skip_whitespace();
            char c = get();
            if (c == '}') break;
            if (c != ',') return value;
        }

        return value;
    }

    std::shared_ptr<JsonValue> parse_array() {
        auto value = std::make_shared<JsonValue>();
        value->type = JsonType::Array;

        get(); // consume '['
        skip_whitespace();

        if (peek() == ']') {
            get();
            return value;
        }

        while (true) {
            auto element = parse_value();
            value->array_value.push_back(element);

            skip_whitespace();
            char c = get();
            if (c == ']') break;
            if (c != ',') return value;
        }

        return value;
    }

    std::shared_ptr<JsonValue> parse_string() {
        auto value = std::make_shared<JsonValue>();
        value->type = JsonType::String;

        get(); // consume '"'

        std::string result;
        while (pos_ < json_->size()) {
            char c = get();
            if (c == '"') break;
            if (c == '\\' && pos_ < json_->size()) {
                char escaped = get();
                switch (escaped) {
                    case '"': result += '"'; break;
                    case '\\': result += '\\'; break;
                    case '/': result += '/'; break;
                    case 'b': result += '\b'; break;
                    case 'f': result += '\f'; break;
                    case 'n': result += '\n'; break;
                    case 'r': result += '\r'; break;
                    case 't': result += '\t'; break;
                    case 'u': {
                        // Skip unicode escapes for simplicity
                        for (int i = 0; i < 4 && pos_ < json_->size(); i++) get();
                        result += '?';
                        break;
                    }
                    default: result += escaped; break;
                }
            } else {
                result += c;
            }
        }

        value->string_value = result;
        return value;
    }

    std::shared_ptr<JsonValue> parse_number() {
        auto value = std::make_shared<JsonValue>();
        value->type = JsonType::Number;

        size_t start = pos_;
        if (peek() == '-') get();

        while (peek() >= '0' && peek() <= '9') get();

        if (peek() == '.') {
            get();
            while (peek() >= '0' && peek() <= '9') get();
        }

        if (peek() == 'e' || peek() == 'E') {
            get();
            if (peek() == '+' || peek() == '-') get();
            while (peek() >= '0' && peek() <= '9') get();
        }

        std::string num_str = json_->substr(start, pos_ - start);
        value->number_value = std::stod(num_str);
        return value;
    }

    std::shared_ptr<JsonValue> parse_bool() {
        auto value = std::make_shared<JsonValue>();
        value->type = JsonType::Bool;

        if (json_->substr(pos_, 4) == "true") {
            pos_ += 4;
            value->bool_value = true;
        } else if (json_->substr(pos_, 5) == "false") {
            pos_ += 5;
            value->bool_value = false;
        }

        return value;
    }

    std::shared_ptr<JsonValue> parse_null() {
        auto value = std::make_shared<JsonValue>();
        value->type = JsonType::Null;

        if (json_->substr(pos_, 4) == "null") {
            pos_ += 4;
        }

        return value;
    }
};

// ============================================================================
// Base64 Decoder
// ============================================================================

static int base64_decode_char(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static std::vector<uint8_t> base64_decode(const std::string& input) {
    std::vector<uint8_t> output;
    output.reserve(input.size() * 3 / 4);

    int val = 0;
    int bits = 0;

    for (char c : input) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;

        int decoded = base64_decode_char(c);
        if (decoded < 0) continue;

        val = (val << 6) | decoded;
        bits += 6;

        if (bits >= 8) {
            bits -= 8;
            output.push_back(static_cast<uint8_t>((val >> bits) & 0xFF));
        }
    }

    return output;
}

// ============================================================================
// glTF Constants
// ============================================================================

// Component types
static constexpr int GLTF_BYTE = 5120;
static constexpr int GLTF_UNSIGNED_BYTE = 5121;
static constexpr int GLTF_SHORT = 5122;
static constexpr int GLTF_UNSIGNED_SHORT = 5123;
static constexpr int GLTF_UNSIGNED_INT = 5125;
static constexpr int GLTF_FLOAT = 5126;

// GLB magic and chunk types
static constexpr uint32_t GLB_MAGIC = 0x46546C67; // "glTF"
static constexpr uint32_t GLB_CHUNK_JSON = 0x4E4F534A; // "JSON"
static constexpr uint32_t GLB_CHUNK_BIN = 0x004E4942; // "BIN\0"

// ============================================================================
// glTF Data Structures
// ============================================================================

struct GltfBuffer {
    std::vector<uint8_t> data;
    size_t byte_length = 0;
};

struct GltfBufferView {
    int buffer = -1;
    size_t byte_offset = 0;
    size_t byte_length = 0;
    size_t byte_stride = 0;
};

struct GltfAccessor {
    int buffer_view = -1;
    size_t byte_offset = 0;
    int component_type = 0;
    size_t count = 0;
    std::string type; // "SCALAR", "VEC2", "VEC3", "VEC4", "MAT2", "MAT3", "MAT4"
};

struct GltfPrimitive {
    int indices = -1;
    int position = -1;
    int normal = -1;
    int texcoord_0 = -1;
};

struct GltfMesh {
    std::string name;
    std::vector<GltfPrimitive> primitives;
};

struct GltfData {
    std::vector<GltfBuffer> buffers;
    std::vector<GltfBufferView> buffer_views;
    std::vector<GltfAccessor> accessors;
    std::vector<GltfMesh> meshes;
    std::string base_path; // Directory containing the .gltf file
};

// ============================================================================
// Helper Functions
// ============================================================================

static size_t get_component_size(int component_type) {
    switch (component_type) {
        case GLTF_BYTE:
        case GLTF_UNSIGNED_BYTE:
            return 1;
        case GLTF_SHORT:
        case GLTF_UNSIGNED_SHORT:
            return 2;
        case GLTF_UNSIGNED_INT:
        case GLTF_FLOAT:
            return 4;
        default:
            return 0;
    }
}

static size_t get_type_components(const std::string& type) {
    if (type == "SCALAR") return 1;
    if (type == "VEC2") return 2;
    if (type == "VEC3") return 3;
    if (type == "VEC4") return 4;
    if (type == "MAT2") return 4;
    if (type == "MAT3") return 9;
    if (type == "MAT4") return 16;
    return 0;
}

static std::string get_directory(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return ".";
    return path.substr(0, pos);
}

static bool read_file_binary(const std::string& path, std::vector<uint8_t>& out_data) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return false;

    size_t size = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);

    out_data.resize(size);
    file.read(reinterpret_cast<char*>(out_data.data()), size);
    return true;
}

static bool read_file_text(const std::string& path, std::string& out_text) {
    std::ifstream file(path);
    if (!file.is_open()) return false;

    std::stringstream ss;
    ss << file.rdbuf();
    out_text = ss.str();
    return true;
}

// ============================================================================
// Buffer Data Reading
// ============================================================================

static const uint8_t* get_accessor_data(
    const GltfData& gltf,
    int accessor_index,
    size_t& out_count,
    size_t& out_stride
) {
    if (accessor_index < 0 || accessor_index >= static_cast<int>(gltf.accessors.size())) {
        return nullptr;
    }

    const GltfAccessor& accessor = gltf.accessors[accessor_index];
    if (accessor.buffer_view < 0 || accessor.buffer_view >= static_cast<int>(gltf.buffer_views.size())) {
        return nullptr;
    }

    const GltfBufferView& view = gltf.buffer_views[accessor.buffer_view];
    if (view.buffer < 0 || view.buffer >= static_cast<int>(gltf.buffers.size())) {
        return nullptr;
    }

    const GltfBuffer& buffer = gltf.buffers[view.buffer];

    size_t component_size = get_component_size(accessor.component_type);
    size_t num_components = get_type_components(accessor.type);
    size_t element_size = component_size * num_components;

    out_count = accessor.count;
    out_stride = view.byte_stride > 0 ? view.byte_stride : element_size;

    size_t offset = view.byte_offset + accessor.byte_offset;
    if (offset >= buffer.data.size()) {
        return nullptr;
    }

    return buffer.data.data() + offset;
}

static void read_float_data(
    const GltfData& gltf,
    int accessor_index,
    std::vector<float>& out_data,
    size_t expected_components
) {
    size_t count, stride;
    const uint8_t* data = get_accessor_data(gltf, accessor_index, count, stride);
    if (!data) return;

    const GltfAccessor& accessor = gltf.accessors[accessor_index];
    size_t num_components = get_type_components(accessor.type);

    if (num_components != expected_components) return;

    out_data.reserve(out_data.size() + count * num_components);

    for (size_t i = 0; i < count; i++) {
        const uint8_t* element = data + i * stride;

        for (size_t c = 0; c < num_components; c++) {
            float value = 0.0f;

            switch (accessor.component_type) {
                case GLTF_FLOAT: {
                    memcpy(&value, element + c * sizeof(float), sizeof(float));
                    break;
                }
                case GLTF_BYTE: {
                    int8_t v;
                    memcpy(&v, element + c, 1);
                    value = static_cast<float>(v) / 127.0f;
                    break;
                }
                case GLTF_UNSIGNED_BYTE: {
                    uint8_t v = element[c];
                    value = static_cast<float>(v) / 255.0f;
                    break;
                }
                case GLTF_SHORT: {
                    int16_t v;
                    memcpy(&v, element + c * 2, 2);
                    value = static_cast<float>(v) / 32767.0f;
                    break;
                }
                case GLTF_UNSIGNED_SHORT: {
                    uint16_t v;
                    memcpy(&v, element + c * 2, 2);
                    value = static_cast<float>(v) / 65535.0f;
                    break;
                }
                default:
                    break;
            }

            out_data.push_back(value);
        }
    }
}

static void read_index_data(
    const GltfData& gltf,
    int accessor_index,
    std::vector<uint32_t>& out_indices,
    uint32_t vertex_offset
) {
    size_t count, stride;
    const uint8_t* data = get_accessor_data(gltf, accessor_index, count, stride);
    if (!data) return;

    const GltfAccessor& accessor = gltf.accessors[accessor_index];

    out_indices.reserve(out_indices.size() + count);

    for (size_t i = 0; i < count; i++) {
        const uint8_t* element = data + i * stride;
        uint32_t index = 0;

        switch (accessor.component_type) {
            case GLTF_UNSIGNED_BYTE: {
                index = element[0];
                break;
            }
            case GLTF_UNSIGNED_SHORT: {
                uint16_t v;
                memcpy(&v, element, 2);
                index = v;
                break;
            }
            case GLTF_UNSIGNED_INT: {
                memcpy(&index, element, 4);
                break;
            }
            default:
                break;
        }

        out_indices.push_back(vertex_offset + index);
    }
}

// ============================================================================
// JSON Parsing
// ============================================================================

static bool parse_gltf_json(const std::string& json_str, GltfData& gltf) {
    JsonParser parser;
    auto root = parser.parse(json_str);

    if (!root || !root->is_object()) {
        return false;
    }

    // Parse buffers
    if (root->has("buffers")) {
        const JsonArray& buffers = (*root)["buffers"].as_array();
        for (size_t i = 0; i < buffers.size(); i++) {
            const JsonValue& buf = *buffers[i];
            GltfBuffer buffer;
            buffer.byte_length = static_cast<size_t>(buf["byteLength"].as_number());

            // Check for embedded base64 data
            if (buf.has("uri")) {
                const std::string& uri = buf["uri"].as_string();
                if (uri.substr(0, 5) == "data:") {
                    // Data URI - extract base64 part
                    size_t comma_pos = uri.find(',');
                    if (comma_pos != std::string::npos) {
                        std::string base64_data = uri.substr(comma_pos + 1);
                        buffer.data = base64_decode(base64_data);
                    }
                } else {
                    // External file
                    std::string buffer_path = gltf.base_path + "/" + uri;
                    read_file_binary(buffer_path, buffer.data);
                }
            }
            // For GLB, buffer 0 data is set separately

            gltf.buffers.push_back(buffer);
        }
    }

    // Parse buffer views
    if (root->has("bufferViews")) {
        const JsonArray& views = (*root)["bufferViews"].as_array();
        for (size_t i = 0; i < views.size(); i++) {
            const JsonValue& view = *views[i];
            GltfBufferView bv;
            bv.buffer = view["buffer"].as_int(-1);
            bv.byte_offset = static_cast<size_t>(view["byteOffset"].as_number(0));
            bv.byte_length = static_cast<size_t>(view["byteLength"].as_number(0));
            bv.byte_stride = static_cast<size_t>(view["byteStride"].as_number(0));
            gltf.buffer_views.push_back(bv);
        }
    }

    // Parse accessors
    if (root->has("accessors")) {
        const JsonArray& accessors = (*root)["accessors"].as_array();
        for (size_t i = 0; i < accessors.size(); i++) {
            const JsonValue& acc = *accessors[i];
            GltfAccessor accessor;
            accessor.buffer_view = acc["bufferView"].as_int(-1);
            accessor.byte_offset = static_cast<size_t>(acc["byteOffset"].as_number(0));
            accessor.component_type = acc["componentType"].as_int(0);
            accessor.count = static_cast<size_t>(acc["count"].as_number(0));
            accessor.type = acc["type"].as_string();
            gltf.accessors.push_back(accessor);
        }
    }

    // Parse meshes
    if (root->has("meshes")) {
        const JsonArray& meshes = (*root)["meshes"].as_array();
        for (size_t i = 0; i < meshes.size(); i++) {
            const JsonValue& mesh_json = *meshes[i];
            GltfMesh mesh;

            if (mesh_json.has("name")) {
                mesh.name = mesh_json["name"].as_string();
            }

            if (mesh_json.has("primitives")) {
                const JsonArray& primitives = mesh_json["primitives"].as_array();
                for (size_t j = 0; j < primitives.size(); j++) {
                    const JsonValue& prim = *primitives[j];
                    GltfPrimitive primitive;

                    if (prim.has("indices")) {
                        primitive.indices = prim["indices"].as_int(-1);
                    }

                    if (prim.has("attributes")) {
                        const JsonValue& attrs = prim["attributes"];
                        if (attrs.has("POSITION")) {
                            primitive.position = attrs["POSITION"].as_int(-1);
                        }
                        if (attrs.has("NORMAL")) {
                            primitive.normal = attrs["NORMAL"].as_int(-1);
                        }
                        if (attrs.has("TEXCOORD_0")) {
                            primitive.texcoord_0 = attrs["TEXCOORD_0"].as_int(-1);
                        }
                    }

                    mesh.primitives.push_back(primitive);
                }
            }

            gltf.meshes.push_back(mesh);
        }
    }

    return true;
}

// ============================================================================
// GLB File Loading
// ============================================================================

static bool load_glb_file(const std::string& path, GltfData& gltf) {
    std::vector<uint8_t> file_data;
    if (!read_file_binary(path, file_data)) {
        return false;
    }

    if (file_data.size() < 12) {
        return false;
    }

    // Read header
    uint32_t magic, version, length;
    memcpy(&magic, file_data.data(), 4);
    memcpy(&version, file_data.data() + 4, 4);
    memcpy(&length, file_data.data() + 8, 4);

    if (magic != GLB_MAGIC) {
        return false;
    }

    if (version != 2) {
        return false; // Only support glTF 2.0
    }

    // Read chunks
    std::string json_str;
    std::vector<uint8_t> bin_data;

    size_t offset = 12;
    while (offset + 8 <= file_data.size()) {
        uint32_t chunk_length, chunk_type;
        memcpy(&chunk_length, file_data.data() + offset, 4);
        memcpy(&chunk_type, file_data.data() + offset + 4, 4);
        offset += 8;

        if (offset + chunk_length > file_data.size()) {
            break;
        }

        if (chunk_type == GLB_CHUNK_JSON) {
            json_str.assign(reinterpret_cast<const char*>(file_data.data() + offset), chunk_length);
        } else if (chunk_type == GLB_CHUNK_BIN) {
            bin_data.assign(file_data.data() + offset, file_data.data() + offset + chunk_length);
        }

        offset += chunk_length;
    }

    if (json_str.empty()) {
        return false;
    }

    gltf.base_path = get_directory(path);

    if (!parse_gltf_json(json_str, gltf)) {
        return false;
    }

    // Set buffer 0 data from GLB binary chunk
    if (!bin_data.empty() && !gltf.buffers.empty()) {
        gltf.buffers[0].data = std::move(bin_data);
    }

    return true;
}

// ============================================================================
// glTF File Loading
// ============================================================================

static bool load_gltf_file(const std::string& path, GltfData& gltf) {
    std::string json_str;
    if (!read_file_text(path, json_str)) {
        return false;
    }

    gltf.base_path = get_directory(path);

    return parse_gltf_json(json_str, gltf);
}

// ============================================================================
// Normal Generation
// ============================================================================

static void compute_face_normal(
    const std::vector<float>& positions,
    uint32_t i0, uint32_t i1, uint32_t i2,
    float& nx, float& ny, float& nz
) {
    float ax = positions[i0 * 3 + 0];
    float ay = positions[i0 * 3 + 1];
    float az = positions[i0 * 3 + 2];

    float bx = positions[i1 * 3 + 0];
    float by = positions[i1 * 3 + 1];
    float bz = positions[i1 * 3 + 2];

    float cx = positions[i2 * 3 + 0];
    float cy = positions[i2 * 3 + 1];
    float cz = positions[i2 * 3 + 2];

    float e1x = bx - ax, e1y = by - ay, e1z = bz - az;
    float e2x = cx - ax, e2y = cy - ay, e2z = cz - az;

    nx = e1y * e2z - e1z * e2y;
    ny = e1z * e2x - e1x * e2z;
    nz = e1x * e2y - e1y * e2x;

    float len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (len > 1e-8f) {
        nx /= len;
        ny /= len;
        nz /= len;
    } else {
        nx = 0.0f;
        ny = 1.0f;
        nz = 0.0f;
    }
}

static void generate_normals(RawMesh& mesh) {
    size_t vertex_count = mesh.positions.size() / 3;
    std::vector<float> accum_normals(vertex_count * 3, 0.0f);

    for (size_t i = 0; i < mesh.indices.size(); i += 3) {
        uint32_t i0 = mesh.indices[i + 0];
        uint32_t i1 = mesh.indices[i + 1];
        uint32_t i2 = mesh.indices[i + 2];

        float nx, ny, nz;
        compute_face_normal(mesh.positions, i0, i1, i2, nx, ny, nz);

        accum_normals[i0 * 3 + 0] += nx;
        accum_normals[i0 * 3 + 1] += ny;
        accum_normals[i0 * 3 + 2] += nz;

        accum_normals[i1 * 3 + 0] += nx;
        accum_normals[i1 * 3 + 1] += ny;
        accum_normals[i1 * 3 + 2] += nz;

        accum_normals[i2 * 3 + 0] += nx;
        accum_normals[i2 * 3 + 1] += ny;
        accum_normals[i2 * 3 + 2] += nz;
    }

    mesh.normals.resize(vertex_count * 3);
    for (size_t i = 0; i < vertex_count; i++) {
        float nx = accum_normals[i * 3 + 0];
        float ny = accum_normals[i * 3 + 1];
        float nz = accum_normals[i * 3 + 2];

        float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len > 1e-8f) {
            mesh.normals[i * 3 + 0] = nx / len;
            mesh.normals[i * 3 + 1] = ny / len;
            mesh.normals[i * 3 + 2] = nz / len;
        } else {
            mesh.normals[i * 3 + 0] = 0.0f;
            mesh.normals[i * 3 + 1] = 1.0f;
            mesh.normals[i * 3 + 2] = 0.0f;
        }
    }
}

// ============================================================================
// Mesh Building
// ============================================================================

static bool build_mesh(const GltfData& gltf, RawMesh& out_mesh) {
    out_mesh.positions.clear();
    out_mesh.normals.clear();
    out_mesh.uvs.clear();
    out_mesh.indices.clear();

    bool has_any_normals = false;

    // Process all meshes and their primitives
    for (const GltfMesh& mesh : gltf.meshes) {
        for (const GltfPrimitive& prim : mesh.primitives) {
            // Must have positions
            if (prim.position < 0) continue;

            uint32_t vertex_offset = static_cast<uint32_t>(out_mesh.positions.size() / 3);

            // Read positions (required)
            size_t positions_start = out_mesh.positions.size();
            read_float_data(gltf, prim.position, out_mesh.positions, 3);
            size_t vertex_count = (out_mesh.positions.size() - positions_start) / 3;

            if (vertex_count == 0) continue;

            // Read normals (optional)
            if (prim.normal >= 0) {
                read_float_data(gltf, prim.normal, out_mesh.normals, 3);
                has_any_normals = true;
            }

            // Pad normals if needed
            while (out_mesh.normals.size() < out_mesh.positions.size()) {
                out_mesh.normals.push_back(0.0f);
                out_mesh.normals.push_back(1.0f);
                out_mesh.normals.push_back(0.0f);
            }

            // Read UVs (optional)
            if (prim.texcoord_0 >= 0) {
                read_float_data(gltf, prim.texcoord_0, out_mesh.uvs, 2);
            }

            // Pad UVs if needed
            while (out_mesh.uvs.size() / 2 < out_mesh.positions.size() / 3) {
                out_mesh.uvs.push_back(0.0f);
                out_mesh.uvs.push_back(0.0f);
            }

            // Read indices or generate them
            if (prim.indices >= 0) {
                read_index_data(gltf, prim.indices, out_mesh.indices, vertex_offset);
            } else {
                // Generate indices for non-indexed geometry
                for (uint32_t i = 0; i < vertex_count; i++) {
                    out_mesh.indices.push_back(vertex_offset + i);
                }
            }
        }
    }

    if (out_mesh.positions.empty()) {
        return false;
    }

    // Generate normals if none were provided
    if (!has_any_normals && !out_mesh.indices.empty()) {
        generate_normals(out_mesh);
    }

    return true;
}

// ============================================================================
// Public API
// ============================================================================

static bool has_extension(const std::string& path, const std::string& ext) {
    if (path.length() < ext.length()) return false;
    std::string path_ext = path.substr(path.length() - ext.length());
    std::transform(path_ext.begin(), path_ext.end(), path_ext.begin(), ::tolower);
    return path_ext == ext;
}

bool load_gltf(const std::string& path, RawMesh& out_mesh) {
    GltfData gltf;

    bool success = false;
    if (has_extension(path, ".glb")) {
        success = load_glb_file(path, gltf);
    } else if (has_extension(path, ".gltf")) {
        success = load_gltf_file(path, gltf);
    } else {
        return false;
    }

    if (!success) {
        return false;
    }

    return build_mesh(gltf, out_mesh);
}

} // namespace vgeo
