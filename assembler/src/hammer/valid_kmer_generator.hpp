#ifndef HAMMER_VALIDKMERGENERATOR_HPP_
#define HAMMER_VALIDKMERGENERATOR_HPP_
#include <stdint.h>
#include <cmath>
#include <string>
#include <vector>
#include "read/read.hpp"
#include "sequence/seq.hpp"
#include "position_read.hpp"
/**
 * This class is designed to iterate through valid k-mers in read.
 * @example
 *   ValidKMerGenerator<2> gen(read, 4);
 *   while (gen.HasMore()) {
 *     MyTrickyFunction(gen.kmer());
 *     gen.Next();
 *   }
 *   or
 *   for (ValidKMerGenerator<2> gen(read, 2); gen.HasMore; gen.Next() {
 *     MyTrickyFunction(gen.kmer(), gen.pos(), gen.correct_probability());
 *   }
 * @param kK k-mer length.
 */
template<uint32_t kK>
class ValidKMerGenerator {
 public:
  /**
   * @param read Read to generate k-mers from.
   * @param bad_quality_threshold  This class virtually cuts
   * nucleotides with quality lower the threshold from the ends of the
   * read. 
   */
  explicit ValidKMerGenerator(const Read &read,
                              uint32_t bad_quality_threshold = 2) :
      bad_quality_threshold_(bad_quality_threshold),
      pos_(-1),
      end_(-1),
      has_more_(true),
      correct_probability_(1),
      first(true),
      kmer_(),
      seq_(read.getSequenceString()),
      qual_(read.getQualityString()) {
    TrimBadQuality();
    Next();
  }
  /**
   * @param seq sequence to generate k-mers from.
   * @param qual quality string
   * @param bad_quality_threshold  This class virtually cuts
   * nucleotides with quality lower the threshold from the ends of the
   * read.
   */
  explicit ValidKMerGenerator(const string & seq, const string & qual,
                              uint32_t bad_quality_threshold = 2) :
      bad_quality_threshold_(bad_quality_threshold),
      pos_(-1),
      end_(-1),
      has_more_(true),
      correct_probability_(1),
      first(true),
      kmer_(),
      seq_(seq),
      qual_(qual) {
    TrimBadQuality();
    Next();
  }
  /**
   * @result true if Next() succeed while generating new k-mer, false
   * otherwise. 
   */
  bool HasMore() const {
    return has_more_;
  }
  /**
   * @result last k-mer generated by Next().
   */
  const Seq<kK>& kmer() {
    return kmer_;
  }
  /**
   * @result last k-mer position in initial read.
   */
  int pos() {
    return pos_;
  }
  /**
   * @result probability that last generated k-mer is correct.
   */
  double correct_probability() {
    return correct_probability_;
  }
  /**
   * This functions reads next k-mer from the read and sets hasmore to
   * if succeeded. You can access k-mer read with kmer().
   */
  void Next();
 private:
  void TrimBadQuality();
  double Prob(uint8_t qual) {
    if (qual < 3) {
      return 0.25;
    }
    static std::vector<double> prob(255, -1);
    if (prob[qual] < -0.1) {
      prob[qual] = 1 - pow(10.0, - qual / 10.0);
    }
    return prob[qual];
  }
  uint32_t GetQual(uint32_t pos) {
    if (qual_.size() <= pos) {
      return 2;
    } else {
      return qual_[pos];
    }
  }
  uint32_t bad_quality_threshold_;
  uint32_t pos_;
  uint32_t end_;
  bool has_more_;
  double correct_probability_;
  bool first;
  Seq<kK> kmer_;
  const std::string &seq_;
  const std::string &qual_;
  // Disallow copy and assign
  ValidKMerGenerator(const ValidKMerGenerator&);
  void operator=(const ValidKMerGenerator&);
};

template<uint32_t kK>
void ValidKMerGenerator<kK>::TrimBadQuality() {
  pos_ = 0;
  for (; pos_ < seq_.size(); ++pos_) {
    if (GetQual(pos_) >= bad_quality_threshold_)
      break;
  }
  end_ = seq_.size();
  for (; end_ > pos_; --end_) {
    if (GetQual(end_ - 1) >= bad_quality_threshold_)
      break;
  }
}

template<uint32_t kK>
void ValidKMerGenerator<kK>::Next() {
  if (pos_ + kK > end_) {
    has_more_ = false;
  } else if (first || !is_nucl(seq_[pos_ + kK - 1])) {
    // in this case we have to look for new k-mer
    correct_probability_ = 1;
    uint32_t start_hypothesis = pos_;
    uint32_t i = pos_;
    for (; i < seq_.size(); ++i) {
      if (i == kK + start_hypothesis) {
        break;
      }
      correct_probability_ *= Prob(GetQual(i));
      if (!is_nucl(seq_[i])) {
        start_hypothesis = i + 1;
        correct_probability_ = 1;
      }
    }
    if (i == kK + start_hypothesis) {
      kmer_ = Seq<kK>(seq_.data() + start_hypothesis, false);
      pos_ = start_hypothesis + 1;
    } else {
      has_more_ = false;
    }
  } else {
    // good case we can just shift our previous answer
    kmer_ = kmer_ << seq_[pos_ + kK - 1];
    correct_probability_ *= Prob(GetQual(pos_ + kK - 1));
    correct_probability_ /= Prob(GetQual(pos_ - 1));
    ++pos_;
  }
  first = false;
}
#endif  // HAMMER_VALIDKMERGENERATOR_HPP__
