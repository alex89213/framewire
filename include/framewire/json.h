/*
 * Description: Small recursive descent JSON reader and writer sized for the mpv
 *   IPC protocol, with a flat node array so parsing a frame costs no
 *   per node allocation.
 * Author: Alex Wu
 * Dependencies:
 * Usage:
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace framewire {

enum class JsonType : uint8_t { Null, Bool, Number, String, Array, Object };

/*
 * One parsed value.
 *
 * Nodes live in a single flat vector and point at each other by index rather
 * than by pointer. A vector of nodes stays warm in cache while walking a
 * document, and growing the vector never invalidates a stored index the way a
 * stored pointer would.
 */
struct JsonNode {
  JsonType type = JsonType::Null;
  bool boolean = false;
  double number = 0.0;

  // byte range inside JsonDoc::text_pool_ holding the decoded string value
  uint32_t str_off = 0;
  uint32_t str_len = 0;

  // byte range holding the member name, only set for members of an object
  uint32_t key_off = 0;
  uint32_t key_len = 0;

  int32_t first_child = -1;
  int32_t next_sibling = -1;
  int32_t child_count = 0;
};

class JsonDoc;

/*
 * Borrowed handle to one node in a document.
 *
 * Every accessor is total, so a missing key gives back an invalid value rather
 * than throwing. Walking mpv responses turns into a chain of lookups with one
 * validity check at the end, instead of a check between every step.
 */
class JsonValue {
 public:
  JsonValue() = default;
  JsonValue(const JsonDoc* doc, int32_t index) : doc_(doc), index_(index) {}

  bool valid() const { return doc_ != nullptr && index_ >= 0; }
  JsonType type() const;

  bool is_null() const { return type() == JsonType::Null; }
  bool is_bool() const { return type() == JsonType::Bool; }
  bool is_number() const { return type() == JsonType::Number; }
  bool is_string() const { return type() == JsonType::String; }
  bool is_array() const { return type() == JsonType::Array; }
  bool is_object() const { return type() == JsonType::Object; }

  // Number of members for an object or elements for an array, else zero.
  size_t size() const;

  /*
   * Looks up a member of an object.
   *
   * Args:
   *   key: Member name to find.
   * Returns:
   *   The member value, or an invalid value when the key is absent.
   */
  JsonValue operator[](std::string_view key) const;

  /*
   * Indexes into an array.
   *
   * Args:
   *   index: Zero based element position.
   * Returns:
   *   The element, or an invalid value when out of range.
   */
  JsonValue operator[](size_t index) const;

  double AsDouble(double fallback = 0.0) const;
  int64_t AsInt(int64_t fallback = 0) const;
  bool AsBool(bool fallback = false) const;
  std::string_view AsString(std::string_view fallback = {}) const;

  // Member name when the value is an object member, else empty.
  std::string_view key() const;

  // First element of an array or object, for iteration by sibling.
  JsonValue FirstChild() const;
  JsonValue NextSibling() const;

 private:
  const JsonDoc* doc_ = nullptr;
  int32_t index_ = -1;
};

/*
 * Owns the nodes and decoded text for one parsed document.
 *
 * A single instance is meant to be reused across messages. Parse clears the
 * buffers but keeps the capacity, so a steady stream of frames settles into
 * zero allocations after the first few messages.
 */
class JsonDoc {
 public:
  /*
   * Parses a complete JSON document.
   *
   * Args:
   *   text: The document bytes, need not be null terminated.
   * Returns:
   *   True on success. On failure the error is available from error().
   */
  bool Parse(std::string_view text);

  JsonValue root() const { return JsonValue(this, nodes_.empty() ? -1 : 0); }
  const std::string& error() const { return error_; }

  const JsonNode& node(int32_t i) const { return nodes_[static_cast<size_t>(i)]; }
  size_t node_count() const { return nodes_.size(); }

  std::string_view Text(uint32_t off, uint32_t len) const {
    return std::string_view(text_pool_.data() + off, len);
  }

 private:
  bool ParseValue(int32_t* out_index, unsigned depth);
  bool ParseObject(int32_t index, unsigned depth);
  bool ParseArray(int32_t index, unsigned depth);
  bool ParseString(uint32_t* off, uint32_t* len);
  bool ParseNumber(int32_t index);
  bool ParseLiteral(std::string_view literal);

  void SkipWhitespace();
  bool AtEnd() const { return pos_ >= src_.size(); }
  char Peek() const { return src_[pos_]; }
  bool Fail(const char* message);
  int32_t NewNode(JsonType type);
  bool AppendUtf8(uint32_t code_point);

  std::vector<JsonNode> nodes_;
  std::string text_pool_;
  std::string error_;
  std::string_view src_;
  size_t pos_ = 0;
};

/*
 * Appends a JSON escaped string, including the surrounding quotes.
 *
 * Args:
 *   out: Destination buffer, appended to.
 *   value: Raw text to escape.
 */
void JsonEscapeTo(std::string& out, std::string_view value);

/*
 * Builds an mpv IPC command line, newline terminated and ready to send.
 *
 * Args:
 *   args: Command words, sent as the elements of the command array.
 *   request_id: Value echoed back by mpv in the reply.
 * Returns:
 *   A single line of JSON ending in a newline.
 */
std::string BuildMpvCommand(const std::vector<std::string>& args, int64_t request_id);

}  // namespace framewire
