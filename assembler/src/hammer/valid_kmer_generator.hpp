#ifndef HAMMER_VALIDKMERGENERATOR_HPP_
#define HAMMER_VALIDKMERGENERATOR_HPP_
/**
 * This class is designed to iterate through valid k-mers in read.
 * @example
 *   ValidKMerGenerator<2> gen(read, 4);
 *   while (gen.HasMore()) {
 *     MyTrickyFunction(gen.kmer());
 *     gen.Next();
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
  explicit ValidKMerGenerator(const Read &read, uint32_t bad_quality_threshold = 2) :
      bad_quality_threshold_(bad_quality_threshold),
      pos_(-1),
      end_(-1),
      has_more_(true),
      first(true),
      kmer_(),
      seq_(read.getSequenceString()),
      qual_(read.getQualityString()){
    TrimBadQuality();
    Next();
  }
  /**
   * @result true if Next() succeed while generating new k-mer, false otherwise.
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
   * This functions reads next k-mer from the read and sets hasmore to
   * if succeeded. You can access k-mer read with kmer().
   */
  void Next();
 private:
  void TrimBadQuality();
  uint32_t bad_quality_threshold_;
  uint32_t pos_;
  uint32_t end_;
  bool has_more_;
  bool first;
  Seq<kK> kmer_;
  const std::string &seq_;
  const std::string &qual_;
};

template<uint32_t kK>
void ValidKMerGenerator<kK>::TrimBadQuality() {
  pos_ = 0;
  for (; pos_ < qual_.size(); ++pos_) {
    if ((uint32_t)qual_[pos_] > bad_quality_threshold_)
      break;
  }
  end_ = qual_.size();
  for (; end_ > pos_; --end_) {
    if ((uint32_t)qual_[end_ - 1] > bad_quality_threshold_)
      break;
  }
}

template<uint32_t kK>
void ValidKMerGenerator<kK>::Next() {
  if (pos_ + kK > end_) {
    has_more_ = false;
  } else if (first || !is_nucl(seq_[pos_ + kK - 1])) {
    // in this case we have to look for new k-mer
    uint32_t start_hypothesis = pos_;
    uint32_t i = pos_;
    for (; i < seq_.size(); ++i) {
      if (i == kK + start_hypothesis) {
        break;
      }
      if (!is_nucl(seq_[i])) {
        start_hypothesis = i + 1;
      }
    }
    if (i == kK + start_hypothesis) {
      kmer_ = Seq<kK>(seq_.data() + start_hypothesis, false);
      pos_ = start_hypothesis + 1;
    } else {
      has_more_ = false;
    }
  } else {
    // good case we can just cyclic shift our answer
    kmer_ = kmer_ << seq_[pos_ + kK - 1];
    ++pos_;
  }
  first = false;
}
#endif //  HAMMER_VALIDKMERGENERATOR_HPP__
