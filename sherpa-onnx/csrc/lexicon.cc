// sherpa-onnx/csrc/lexicon.cc
//
// Copyright (c)  2022-2023  Xiaomi Corporation

#include "sherpa-onnx/csrc/lexicon.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <utility>

#if __ANDROID_API__ >= 9
#include <strstream>

#include "android/asset_manager.h"
#include "android/asset_manager_jni.h"
#endif

#include "sherpa-onnx/csrc/macros.h"
#include "sherpa-onnx/csrc/onnx-utils.h"
#include "sherpa-onnx/csrc/text-utils.h"

namespace sherpa_onnx {

static void ToLowerCase(std::string *in_out) {
  std::transform(in_out->begin(), in_out->end(), in_out->begin(),
                 [](unsigned char c) { return std::tolower(c); });
}

// Note: We don't use SymbolTable here since tokens may contain a blank
// in the first column
static std::unordered_map<std::string, int32_t> ReadTokens(std::istream &is) {
  std::unordered_map<std::string, int32_t> token2id;

  std::string line;

  std::string sym;
  int32_t id;
  while (std::getline(is, line)) {
    std::istringstream iss(line);
    iss >> sym;
    if (iss.eof()) {
      id = atoi(sym.c_str());
      sym = " ";
    } else {
      iss >> id;
    }

    if (!iss.eof()) {
      SHERPA_ONNX_LOGE("Error: %s", line.c_str());
      exit(-1);
    }

#if 0
    if (token2id.count(sym)) {
      SHERPA_ONNX_LOGE("Duplicated token %s. Line %s. Existing ID: %d",
                       sym.c_str(), line.c_str(), token2id.at(sym));
      exit(-1);
    }
#endif
    token2id.insert({std::move(sym), id});
  }

  return token2id;
}

static std::vector<int32_t> ConvertTokensToIds(
    const std::unordered_map<std::string, int32_t> &token2id,
    const std::vector<std::string> &tokens) {
  std::vector<int32_t> ids;
  ids.reserve(tokens.size());
  for (const auto &s : tokens) {
    if (!token2id.count(s)) {
      return {};
    }
    int32_t id = token2id.at(s);
    ids.push_back(id);
  }

  return ids;
}

Lexicon::Lexicon(const std::string &lexicon, const std::string &tokens,
                 const std::string &punctuations, const std::string &language,
                 bool debug /*= false*/)
    : debug_(debug) {
  InitLanguage(language);

  {
    std::ifstream is(tokens);
    InitTokens(is);
  }

  {
    std::ifstream is(lexicon);
    InitLexicon(is);
  }

  InitPunctuations(punctuations);
}

#if __ANDROID_API__ >= 9
Lexicon::Lexicon(AAssetManager *mgr, const std::string &lexicon,
                 const std::string &tokens, const std::string &punctuations,
                 const std::string &language, bool debug /*= false*/)
    : debug_(debug) {
  InitLanguage(language);

  {
    auto buf = ReadFile(mgr, tokens);
    std::istrstream is(buf.data(), buf.size());
    InitTokens(is);
  }

  {
    auto buf = ReadFile(mgr, lexicon);
    std::istrstream is(buf.data(), buf.size());
    InitLexicon(is);
  }

  InitPunctuations(punctuations);
}
#endif

std::vector<int64_t> Lexicon::ConvertTextToTokenIds(
    const std::string &text) const {
  switch (language_) {
    case Language::kEnglish:
      return ConvertTextToTokenIdsEnglish(text);
    case Language::kChinese:
      return ConvertTextToTokenIdsChinese(text);
    default:
      SHERPA_ONNX_LOGE("Unknonw language: %d", static_cast<int32_t>(language_));
      exit(-1);
  }

  return {};
}

std::vector<int64_t> Lexicon::ConvertTextToTokenIdsChinese(
    const std::string &text) const {
  std::vector<std::string> words = SplitUtf8(text);

  if (debug_) {
    fprintf(stderr, "Input text in string: %s\n", text.c_str());
    fprintf(stderr, "Input text in bytes:");
    for (uint8_t c : text) {
      fprintf(stderr, " %02x", c);
    }
    fprintf(stderr, "\n");
    fprintf(stderr, "After splitting to words:");
    for (const auto &w : words) {
      fprintf(stderr, " %s", w.c_str());
    }
    fprintf(stderr, "\n");
  }

  std::vector<int64_t> ans;

  auto sil = token2id_.at("sil");
  auto eos = token2id_.at("eos");

  ans.push_back(sil);

  for (const auto &w : words) {
    if (punctuations_.count(w)) {
      ans.push_back(sil);
      continue;
    }

    if (!word2ids_.count(w)) {
      SHERPA_ONNX_LOGE("OOV %s. Ignore it!", w.c_str());
      continue;
    }

    const auto &token_ids = word2ids_.at(w);
    ans.insert(ans.end(), token_ids.begin(), token_ids.end());
  }
  ans.push_back(sil);
  ans.push_back(eos);
  return ans;
}

std::vector<int64_t> Lexicon::ConvertTextToTokenIdsEnglish(
    const std::string &_text) const {
  std::string text(_text);
  ToLowerCase(&text);

  std::vector<std::string> words = SplitUtf8(text);

  if (debug_) {
    fprintf(stderr, "Input text (lowercase) in string: %s\n", text.c_str());
    fprintf(stderr, "Input text in bytes:");
    for (uint8_t c : text) {
      fprintf(stderr, " %02x", c);
    }
    fprintf(stderr, "\n");
    fprintf(stderr, "After splitting to words:");
    for (const auto &w : words) {
      fprintf(stderr, " %s", w.c_str());
    }
    fprintf(stderr, "\n");
  }

  int32_t blank = token2id_.at(" ");

  std::vector<int64_t> ans;
  for (const auto &w : words) {
    if (punctuations_.count(w)) {
      ans.push_back(token2id_.at(w));
      continue;
    }

    if (!word2ids_.count(w)) {
      SHERPA_ONNX_LOGE("OOV %s. Ignore it!", w.c_str());
      continue;
    }

    const auto &token_ids = word2ids_.at(w);
    ans.insert(ans.end(), token_ids.begin(), token_ids.end());
    ans.push_back(blank);
  }

  if (!ans.empty()) {
    // remove the last blank
    ans.resize(ans.size() - 1);
  }

  return ans;
}

void Lexicon::InitTokens(std::istream &is) { token2id_ = ReadTokens(is); }

void Lexicon::InitLanguage(const std::string &_lang) {
  std::string lang(_lang);
  ToLowerCase(&lang);
  if (lang == "english") {
    language_ = Language::kEnglish;
  } else if (lang == "chinese") {
    language_ = Language::kChinese;
  } else {
    SHERPA_ONNX_LOGE("Unknown language: %s", _lang.c_str());
    exit(-1);
  }
}

void Lexicon::InitLexicon(std::istream &is) {
  std::string word;
  std::vector<std::string> token_list;
  std::string line;
  std::string phone;

  while (std::getline(is, line)) {
    std::istringstream iss(line);

    token_list.clear();

    iss >> word;
    ToLowerCase(&word);

    if (word2ids_.count(word)) {
      SHERPA_ONNX_LOGE("Duplicated word: %s", word.c_str());
      return;
    }

    while (iss >> phone) {
      token_list.push_back(std::move(phone));
    }

    std::vector<int32_t> ids = ConvertTokensToIds(token2id_, token_list);
    if (ids.empty()) {
      continue;
    }
    word2ids_.insert({std::move(word), std::move(ids)});
  }
}

void Lexicon::InitPunctuations(const std::string &punctuations) {
  std::vector<std::string> punctuation_list;
  SplitStringToVector(punctuations, " ", false, &punctuation_list);
  for (auto &s : punctuation_list) {
    punctuations_.insert(std::move(s));
  }
}

}  // namespace sherpa_onnx
