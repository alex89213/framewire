/*
 * Description: Recursive descent JSON parser and the small writer helpers used
 *   to talk to the mpv IPC socket.
 * Author: Alex Wu
 * Dependencies: framewire/json.h
 * Usage:
 */

#include "framewire/json.h"

#include <charconv>
#include <cstdio>
#include <cstring>

namespace framewire {
namespace {

// mpv replies nest only a few levels deep. the cap turns a corrupt or hostile
// message into a clean parse error instead of a blown stack
constexpr unsigned kMaxDepth = 32;

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

int HexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

JsonType JsonValue::type() const {
  return valid() ? doc_->node(index_).type : JsonType::Null;
}

size_t JsonValue::size() const {
  if (!valid()) return 0;
  const JsonNode& n = doc_->node(index_);
  if (n.type != JsonType::Array && n.type != JsonType::Object) return 0;
  return static_cast<size_t>(n.child_count);
}

JsonValue JsonValue::operator[](std::string_view key) const {
  if (!valid()) return {};
  const JsonNode& n = doc_->node(index_);
  if (n.type != JsonType::Object) return {};

  // linear scan is the right call here, mpv objects hold a handful of keys and
  // a hash map would cost more to build than the scan saves
  for (int32_t c = n.first_child; c >= 0; c = doc_->node(c).next_sibling) {
    const JsonNode& child = doc_->node(c);
    if (doc_->Text(child.key_off, child.key_len) == key) return JsonValue(doc_, c);
  }
  return {};
}

JsonValue JsonValue::operator[](size_t index) const {
  if (!valid()) return {};
  const JsonNode& n = doc_->node(index_);
  if (n.type != JsonType::Array && n.type != JsonType::Object) return {};
  if (index >= static_cast<size_t>(n.child_count)) return {};

  int32_t c = n.first_child;
  for (size_t i = 0; i < index && c >= 0; ++i) c = doc_->node(c).next_sibling;
  return c >= 0 ? JsonValue(doc_, c) : JsonValue();
}

double JsonValue::AsDouble(double fallback) const {
  if (!valid()) return fallback;
  const JsonNode& n = doc_->node(index_);
  if (n.type == JsonType::Number) return n.number;
  if (n.type == JsonType::Bool) return n.boolean ? 1.0 : 0.0;
  return fallback;
}

int64_t JsonValue::AsInt(int64_t fallback) const {
  if (!valid()) return fallback;
  const JsonNode& n = doc_->node(index_);
  if (n.type == JsonType::Number) return static_cast<int64_t>(n.number);
  if (n.type == JsonType::Bool) return n.boolean ? 1 : 0;
  return fallback;
}

bool JsonValue::AsBool(bool fallback) const {
  if (!valid()) return fallback;
  const JsonNode& n = doc_->node(index_);
  if (n.type == JsonType::Bool) return n.boolean;
  if (n.type == JsonType::Number) return n.number != 0.0;
  return fallback;
}

std::string_view JsonValue::AsString(std::string_view fallback) const {
  if (!valid()) return fallback;
  const JsonNode& n = doc_->node(index_);
  if (n.type != JsonType::String) return fallback;
  return doc_->Text(n.str_off, n.str_len);
}

std::string_view JsonValue::key() const {
  if (!valid()) return {};
  const JsonNode& n = doc_->node(index_);
  return doc_->Text(n.key_off, n.key_len);
}

JsonValue JsonValue::FirstChild() const {
  if (!valid()) return {};
  const int32_t c = doc_->node(index_).first_child;
  return c >= 0 ? JsonValue(doc_, c) : JsonValue();
}

JsonValue JsonValue::NextSibling() const {
  if (!valid()) return {};
  const int32_t s = doc_->node(index_).next_sibling;
  return s >= 0 ? JsonValue(doc_, s) : JsonValue();
}

bool JsonDoc::Fail(const char* message) {
  char buf[128];
  std::snprintf(buf, sizeof(buf), "%s at byte %zu", message, pos_);
  error_ = buf;
  nodes_.clear();
  return false;
}

int32_t JsonDoc::NewNode(JsonType type) {
  nodes_.emplace_back();
  const auto index = static_cast<int32_t>(nodes_.size() - 1);
  nodes_[static_cast<size_t>(index)].type = type;
  return index;
}

void JsonDoc::SkipWhitespace() {
  while (pos_ < src_.size()) {
    const char c = src_[pos_];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      ++pos_;
    } else {
      break;
    }
  }
}

bool JsonDoc::Parse(std::string_view text) {
  // clear keeps the allocated capacity, so a long lived reader stops calling
  // into the allocator once the buffers reach the size of a typical message
  nodes_.clear();
  text_pool_.clear();
  error_.clear();
  src_ = text;
  pos_ = 0;

  int32_t root = -1;
  if (!ParseValue(&root, 0)) return false;

  SkipWhitespace();
  if (!AtEnd()) return Fail("trailing bytes after the document");
  return true;
}

bool JsonDoc::ParseValue(int32_t* out_index, unsigned depth) {
  if (depth > kMaxDepth) return Fail("document nests too deeply");

  SkipWhitespace();
  if (AtEnd()) return Fail("unexpected end of document");

  const char c = Peek();
  switch (c) {
    case '{': {
      const int32_t index = NewNode(JsonType::Object);
      *out_index = index;
      return ParseObject(index, depth);
    }
    case '[': {
      const int32_t index = NewNode(JsonType::Array);
      *out_index = index;
      return ParseArray(index, depth);
    }
    case '"': {
      const int32_t index = NewNode(JsonType::String);
      *out_index = index;
      uint32_t off = 0;
      uint32_t len = 0;
      if (!ParseString(&off, &len)) return false;
      nodes_[static_cast<size_t>(index)].str_off = off;
      nodes_[static_cast<size_t>(index)].str_len = len;
      return true;
    }
    case 't': {
      const int32_t index = NewNode(JsonType::Bool);
      *out_index = index;
      if (!ParseLiteral("true")) return false;
      nodes_[static_cast<size_t>(index)].boolean = true;
      return true;
    }
    case 'f': {
      const int32_t index = NewNode(JsonType::Bool);
      *out_index = index;
      if (!ParseLiteral("false")) return false;
      nodes_[static_cast<size_t>(index)].boolean = false;
      return true;
    }
    case 'n': {
      const int32_t index = NewNode(JsonType::Null);
      *out_index = index;
      return ParseLiteral("null");
    }
    default: {
      if (c == '-' || IsDigit(c)) {
        const int32_t index = NewNode(JsonType::Number);
        *out_index = index;
        return ParseNumber(index);
      }
      return Fail("unexpected character");
    }
  }
}

bool JsonDoc::ParseObject(int32_t index, unsigned depth) {
  ++pos_;  // consume the opening brace
  SkipWhitespace();

  if (!AtEnd() && Peek() == '}') {
    ++pos_;
    return true;
  }

  int32_t last = -1;
  for (;;) {
    SkipWhitespace();
    if (AtEnd() || Peek() != '"') return Fail("expected a quoted member name");

    uint32_t key_off = 0;
    uint32_t key_len = 0;
    if (!ParseString(&key_off, &key_len)) return false;

    SkipWhitespace();
    if (AtEnd() || Peek() != ':') return Fail("expected a colon after the member name");
    ++pos_;

    int32_t child = -1;
    if (!ParseValue(&child, depth + 1)) return false;

    // the key range is recorded on the child, so a member carries the name and
    // the value together and the object needs no parallel key array
    nodes_[static_cast<size_t>(child)].key_off = key_off;
    nodes_[static_cast<size_t>(child)].key_len = key_len;

    if (last < 0) {
      nodes_[static_cast<size_t>(index)].first_child = child;
    } else {
      nodes_[static_cast<size_t>(last)].next_sibling = child;
    }
    last = child;
    ++nodes_[static_cast<size_t>(index)].child_count;

    SkipWhitespace();
    if (AtEnd()) return Fail("unterminated object");
    if (Peek() == ',') {
      ++pos_;
      continue;
    }
    if (Peek() == '}') {
      ++pos_;
      return true;
    }
    return Fail("expected a comma or closing brace");
  }
}

bool JsonDoc::ParseArray(int32_t index, unsigned depth) {
  ++pos_;  // consume the opening bracket
  SkipWhitespace();

  if (!AtEnd() && Peek() == ']') {
    ++pos_;
    return true;
  }

  int32_t last = -1;
  for (;;) {
    int32_t child = -1;
    if (!ParseValue(&child, depth + 1)) return false;

    if (last < 0) {
      nodes_[static_cast<size_t>(index)].first_child = child;
    } else {
      nodes_[static_cast<size_t>(last)].next_sibling = child;
    }
    last = child;
    ++nodes_[static_cast<size_t>(index)].child_count;

    SkipWhitespace();
    if (AtEnd()) return Fail("unterminated array");
    if (Peek() == ',') {
      ++pos_;
      continue;
    }
    if (Peek() == ']') {
      ++pos_;
      return true;
    }
    return Fail("expected a comma or closing bracket");
  }
}

bool JsonDoc::AppendUtf8(uint32_t cp) {
  if (cp <= 0x7F) {
    text_pool_.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FF) {
    text_pool_.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    text_pool_.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0xFFFF) {
    text_pool_.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    text_pool_.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    text_pool_.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0x10FFFF) {
    text_pool_.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    text_pool_.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    text_pool_.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    text_pool_.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    return false;
  }
  return true;
}

bool JsonDoc::ParseString(uint32_t* off, uint32_t* len) {
  ++pos_;  // consume the opening quote
  const auto start = static_cast<uint32_t>(text_pool_.size());

  while (!AtEnd()) {
    const char c = src_[pos_];

    if (c == '"') {
      ++pos_;
      *off = start;
      *len = static_cast<uint32_t>(text_pool_.size()) - start;
      return true;
    }

    if (c != '\\') {
      // control characters below 0x20 are not legal raw inside a string
      if (static_cast<unsigned char>(c) < 0x20) return Fail("raw control character in a string");
      text_pool_.push_back(c);
      ++pos_;
      continue;
    }

    ++pos_;
    if (AtEnd()) return Fail("escape at end of document");
    const char e = src_[pos_++];
    switch (e) {
      case '"': text_pool_.push_back('"'); break;
      case '\\': text_pool_.push_back('\\'); break;
      case '/': text_pool_.push_back('/'); break;
      case 'b': text_pool_.push_back('\b'); break;
      case 'f': text_pool_.push_back('\f'); break;
      case 'n': text_pool_.push_back('\n'); break;
      case 'r': text_pool_.push_back('\r'); break;
      case 't': text_pool_.push_back('\t'); break;
      case 'u': {
        if (pos_ + 4 > src_.size()) return Fail("truncated unicode escape");
        uint32_t cp = 0;
        for (int i = 0; i < 4; ++i) {
          const int d = HexDigit(src_[pos_ + static_cast<size_t>(i)]);
          if (d < 0) return Fail("bad hex digit in a unicode escape");
          cp = (cp << 4) | static_cast<uint32_t>(d);
        }
        pos_ += 4;

        // a high surrogate is only half a code point, so pull in the matching
        // low surrogate before encoding. mpv sends plain ascii in practice,
        // but a file path with an emoji would arrive this way
        if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 6 <= src_.size() && src_[pos_] == '\\' &&
            src_[pos_ + 1] == 'u') {
          uint32_t low = 0;
          bool ok = true;
          for (int i = 0; i < 4; ++i) {
            const int d = HexDigit(src_[pos_ + 2 + static_cast<size_t>(i)]);
            if (d < 0) {
              ok = false;
              break;
            }
            low = (low << 4) | static_cast<uint32_t>(d);
          }
          if (ok && low >= 0xDC00 && low <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            pos_ += 6;
          }
        }

        // an unpaired surrogate cannot be encoded, substitute the replacement
        // character so one odd filename does not kill the whole stream
        if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
        if (!AppendUtf8(cp)) return Fail("code point out of range");
        break;
      }
      default:
        return Fail("unknown escape character");
    }
  }
  return Fail("unterminated string");
}

bool JsonDoc::ParseNumber(int32_t index) {
  const size_t start = pos_;
  if (!AtEnd() && Peek() == '-') ++pos_;

  // JSON allows a single leading zero but not a run of digits after one, so
  // 0 and 0.5 are fine while 01 is not. from_chars would happily accept 01,
  // which would let a malformed message through
  if (AtEnd() || !IsDigit(Peek())) {
    pos_ = start;
    return Fail("number needs at least one digit");
  }
  if (Peek() == '0') {
    ++pos_;
    if (!AtEnd() && IsDigit(Peek())) {
      pos_ = start;
      return Fail("number has a leading zero");
    }
  }

  while (!AtEnd() && IsDigit(Peek())) ++pos_;
  if (!AtEnd() && Peek() == '.') {
    ++pos_;
    while (!AtEnd() && IsDigit(Peek())) ++pos_;
  }
  if (!AtEnd() && (Peek() == 'e' || Peek() == 'E')) {
    ++pos_;
    if (!AtEnd() && (Peek() == '+' || Peek() == '-')) ++pos_;
    while (!AtEnd() && IsDigit(Peek())) ++pos_;
  }

  double value = 0.0;
  const char* first = src_.data() + start;
  const char* last = src_.data() + pos_;
  // from_chars works on a range and never needs a null terminator, so the
  // source view can point straight at the socket buffer with no copy
  const auto result = std::from_chars(first, last, value);
  if (result.ec != std::errc() || result.ptr != last) {
    pos_ = start;
    return Fail("malformed number");
  }

  nodes_[static_cast<size_t>(index)].number = value;
  return true;
}

bool JsonDoc::ParseLiteral(std::string_view literal) {
  if (src_.size() - pos_ < literal.size() || src_.compare(pos_, literal.size(), literal) != 0) {
    return Fail("unknown literal");
  }
  pos_ += literal.size();
  return true;
}

void JsonEscapeTo(std::string& out, std::string_view value) {
  out.push_back('"');
  for (const char c : value) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: {
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c) & 0xFF);
          out += buf;
        } else {
          out.push_back(c);
        }
      }
    }
  }
  out.push_back('"');
}

std::string BuildMpvObserveCommand(int64_t observe_id, std::string_view property,
                                   int64_t request_id) {
  std::string out = "{\"command\":[\"observe_property\",";
  out += std::to_string(observe_id);  // a number, not a quoted string
  out.push_back(',');
  JsonEscapeTo(out, property);
  out += "],\"request_id\":";
  out += std::to_string(request_id);
  out += "}\n";
  return out;
}

std::string BuildMpvCommand(const std::vector<std::string>& args, int64_t request_id) {
  std::string out = "{\"command\":[";
  for (size_t i = 0; i < args.size(); ++i) {
    if (i != 0) out.push_back(',');
    JsonEscapeTo(out, args[i]);
  }
  out += "],\"request_id\":";
  out += std::to_string(request_id);
  out += "}\n";
  return out;
}

}  // namespace framewire
