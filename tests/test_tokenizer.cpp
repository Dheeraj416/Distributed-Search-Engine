#include "dse/tokenizer.hpp"

#include <gtest/gtest.h>

namespace dse {
namespace {

TEST(TokenizerTest, LowercasesAndSplitsOnWhitespace) {
  Tokenizer tokenizer;
  auto tokens = tokenizer.Tokenize("Distributed SEARCH engines");
  std::vector<std::string> expected = {"distributed", "search", "engines"};
  ASSERT_EQ(tokens, expected);
}

TEST(TokenizerTest, DropsStopwordsAndSingleCharacterTokens) {
  Tokenizer tokenizer;
  auto tokens = tokenizer.Tokenize("the cat is on a mat");
  // "the", "is", "on", "a" are stopwords; "cat" and "mat" survive.
  ASSERT_EQ(tokens.size(), 2u);
  EXPECT_EQ(tokens[0], "cat");
  EXPECT_EQ(tokens[1], "mat");
}

TEST(TokenizerTest, StripsPunctuation) {
  Tokenizer tokenizer;
  auto tokens = tokenizer.Tokenize("BM25, ranking! (functions) work well.");
  std::vector<std::string> expected = {"bm25", "ranking", "functions", "work", "well"};
  ASSERT_EQ(tokens, expected);
}

TEST(TokenizerTest, EmptyStringProducesNoTokens) {
  Tokenizer tokenizer;
  EXPECT_TRUE(tokenizer.Tokenize("").empty());
  EXPECT_TRUE(tokenizer.Tokenize("   ").empty());
}

TEST(TokenizerTest, OnlyStopwordsProducesNoTokens) {
  Tokenizer tokenizer;
  EXPECT_TRUE(tokenizer.Tokenize("the a an is").empty());
}

TEST(TokenizerTest, NormalizeTokenLowercasesAndStripsNonAlnum) {
  EXPECT_EQ(Tokenizer::NormalizeToken("Hello!!"), "hello");
  EXPECT_EQ(Tokenizer::NormalizeToken("C++17"), "c17");
  EXPECT_EQ(Tokenizer::NormalizeToken(""), "");
}

}  // namespace
}  // namespace dse
