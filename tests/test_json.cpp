/*
 * Description: Unit tests for the JSON reader and the mpv command writer.
 * Author: Alex Wu
 * Dependencies: framewire core library, tests/test_util.h
 * Usage: ctest, or run the binary directly
 */

#include <string>

#include "framewire/json.h"
#include "test_util.h"

using namespace framewire;

namespace {

void TestScalars() {
  TEST_CASE("scalar documents parse") {
    JsonDoc doc;
    CHECK(doc.Parse("42"));
    CHECK(doc.root().is_number());
    CHECK_EQ(doc.root().AsInt(), 42);

    CHECK(doc.Parse("-17.5"));
    CHECK_NEAR(doc.root().AsDouble(), -17.5, 1e-9);

    CHECK(doc.Parse("1.5e3"));
    CHECK_NEAR(doc.root().AsDouble(), 1500.0, 1e-9);

    // a single leading zero is legal, a run of digits after one is not
    CHECK(doc.Parse("0"));
    CHECK_EQ(doc.root().AsInt(), 0);
    CHECK(doc.Parse("0.5"));
    CHECK_NEAR(doc.root().AsDouble(), 0.5, 1e-9);
    CHECK(doc.Parse("-0.25"));
    CHECK_NEAR(doc.root().AsDouble(), -0.25, 1e-9);
    CHECK(doc.Parse("[0,1,0]"));
    CHECK_EQ(doc.root().size(), 3);

    CHECK(doc.Parse("true"));
    CHECK(doc.root().AsBool());

    CHECK(doc.Parse("false"));
    CHECK(!doc.root().AsBool(true));

    CHECK(doc.Parse("null"));
    CHECK(doc.root().is_null());

    CHECK(doc.Parse("\"hello\""));
    CHECK(doc.root().AsString() == "hello");
  }
}

void TestStrings() {
  TEST_CASE("escapes decode") {
    JsonDoc doc;
    CHECK(doc.Parse(R"("a\"b\\c\/d\ne\tf")"));
    CHECK(doc.root().AsString() == "a\"b\\c/d\ne\tf");

    CHECK(doc.Parse(R"("\u0041\u00e9\u20ac")"));
    CHECK(doc.root().AsString() == "A\u00e9\u20ac");

    // a surrogate pair has to fold back into one code point
    CHECK(doc.Parse(R"("\ud83d\ude00")"));
    CHECK(doc.root().AsString() == "\U0001F600");

    // an unpaired high surrogate becomes the replacement character rather than
    // failing the whole message
    CHECK(doc.Parse(R"("\ud83dx")"));
    CHECK(doc.root().AsString() == "\ufffdx");

    CHECK(doc.Parse("\"\""));
    CHECK(doc.root().AsString().empty());
  }
}

void TestContainers() {
  TEST_CASE("arrays and objects walk correctly") {
    JsonDoc doc;
    CHECK(doc.Parse(R"({"a":1,"b":[10,20,30],"c":{"d":"x"},"e":null})"));

    const JsonValue root = doc.root();
    CHECK(root.is_object());
    CHECK_EQ(root.size(), 4);
    CHECK_EQ(root["a"].AsInt(), 1);

    const JsonValue b = root["b"];
    CHECK(b.is_array());
    CHECK_EQ(b.size(), 3);
    CHECK_EQ(b[0].AsInt(), 10);
    CHECK_EQ(b[2].AsInt(), 30);
    CHECK(!b[3].valid());

    CHECK(root["c"]["d"].AsString() == "x");
    CHECK(root["e"].is_null());

    // a missing key chains without blowing up, which is what lets the producer
    // read a deep path with one check at the end
    CHECK(!root["nope"].valid());
    CHECK(!root["nope"]["deeper"][2].valid());
    CHECK_EQ(root["nope"].AsInt(-1), -1);

    CHECK(doc.Parse("[]"));
    CHECK(doc.root().is_array());
    CHECK_EQ(doc.root().size(), 0);

    CHECK(doc.Parse("{}"));
    CHECK(doc.root().is_object());
    CHECK_EQ(doc.root().size(), 0);
  }

  TEST_CASE("sibling walk visits every member") {
    JsonDoc doc;
    CHECK(doc.Parse(R"({"x":1,"y":2,"z":3})"));

    int count = 0;
    long long sum = 0;
    for (JsonValue v = doc.root().FirstChild(); v.valid(); v = v.NextSibling()) {
      ++count;
      sum += v.AsInt();
      CHECK(!v.key().empty());
    }
    CHECK_EQ(count, 3);
    CHECK_EQ(sum, 6);
  }
}

void TestMalformed() {
  TEST_CASE("malformed input fails instead of crashing") {
    const char* bad[] = {
        "",         "{",        "}",         "[",         "]",        "[1,",
        "[1,]",     "{\"a\":}", "{\"a\"}",   "{a:1}",     "tru",      "nul",
        "\"abc",    "\"\\q\"",  "\"\\u12\"", "\"\\uZZZZ\"", "01",     "1.2.3",
        "{\"a\":1}x", "--5",    "+5",        "[1 2]",
    };
    for (const char* text : bad) {
      JsonDoc doc;
      const bool parsed = doc.Parse(text);
      CHECK(!parsed);
      if (!parsed) CHECK(!doc.error().empty());
    }
  }

  TEST_CASE("deep nesting is refused rather than overflowing") {
    std::string deep;
    for (int i = 0; i < 200; ++i) deep += '[';
    for (int i = 0; i < 200; ++i) deep += ']';

    JsonDoc doc;
    CHECK(!doc.Parse(deep));
  }

  TEST_CASE("a raw control character in a string is refused") {
    JsonDoc doc;
    const std::string with_newline = "\"ab\ncd\"";
    CHECK(!doc.Parse(with_newline));
  }
}

void TestMpvShapes() {
  TEST_CASE("an mpv vo-passes event reads back") {
    const char* msg =
        R"({"event":"property-change","id":1,"name":"vo-passes","data":{"fresh":[)"
        R"({"desc":"upload","last":91000,"avg":90000,"peak":140000},)"
        R"({"desc":"espcn conv1","last":640000,"avg":630000,"peak":1100000}],"redraw":[]}})";

    JsonDoc doc;
    CHECK(doc.Parse(msg));

    const JsonValue root = doc.root();
    CHECK(root["event"].AsString() == "property-change");
    CHECK(root["name"].AsString() == "vo-passes");
    CHECK_EQ(root["id"].AsInt(), 1);

    const JsonValue fresh = root["data"]["fresh"];
    CHECK_EQ(fresh.size(), 2);
    CHECK(fresh[0]["desc"].AsString() == "upload");
    CHECK_EQ(fresh[0]["last"].AsInt(), 91000);
    CHECK_EQ(fresh[1]["last"].AsInt(), 640000);
    CHECK_EQ(root["data"]["redraw"].size(), 0);
  }

  TEST_CASE("an mpv command reply reads back") {
    JsonDoc doc;
    CHECK(doc.Parse(R"({"error":"success","data":null,"request_id":7})"));
    CHECK(doc.root()["error"].AsString() == "success");
    CHECK_EQ(doc.root()["request_id"].AsInt(), 7);
  }

  TEST_CASE("the same document object can be reused") {
    JsonDoc doc;
    for (int i = 0; i < 100; ++i) {
      const std::string msg = R"({"n":)" + std::to_string(i) + "}";
      CHECK(doc.Parse(msg));
      CHECK_EQ(doc.root()["n"].AsInt(), i);
    }
    // a failed parse must not leave usable looking state behind
    CHECK(!doc.Parse("{bad"));
    CHECK(!doc.root().valid());
    CHECK(doc.Parse(R"({"n":5})"));
    CHECK_EQ(doc.root()["n"].AsInt(), 5);
  }
}

void TestWriter() {
  TEST_CASE("escaping and command building round trip") {
    std::string out;
    JsonEscapeTo(out, "plain");
    CHECK(out == "\"plain\"");

    out.clear();
    JsonEscapeTo(out, "quote\" back\\ tab\t");
    CHECK(out == "\"quote\\\" back\\\\ tab\\t\"");

    const std::string cmd = BuildMpvCommand({"observe_property", "1", "vo-passes"}, 7);
    CHECK(cmd == "{\"command\":[\"observe_property\",\"1\",\"vo-passes\"],\"request_id\":7}\n");

    // whatever the writer produces has to parse back cleanly
    JsonDoc doc;
    CHECK(doc.Parse(cmd.substr(0, cmd.size() - 1)));
    CHECK_EQ(doc.root()["command"].size(), 3);
    CHECK(doc.root()["command"][0].AsString() == "observe_property");
  }

  TEST_CASE("a control character survives escaping") {
    std::string out;
    JsonEscapeTo(out, std::string("a\x01"
                                  "b"));
    JsonDoc doc;
    CHECK(doc.Parse(out));
    CHECK_EQ(doc.root().AsString().size(), 3);
  }
}

}  // namespace

int main() {
  std::printf("test_json\n");
  TestScalars();
  TestStrings();
  TestContainers();
  TestMalformed();
  TestMpvShapes();
  TestWriter();
  return fwtest::Finish("test_json");
}
