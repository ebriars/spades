//***************************************************************************
//* Copyright (c) 2016 Saint Petersburg State University
//* All Rights Reserved
//* See file LICENSE for details.
//***************************************************************************

#include "bwa_index.hpp"

#include "bwa/bwa.h"
#include "bwa/bwamem.h"
#include "bwa/rle.h"
#include "bwa/rope.h"
#include "bwa/utils.h"
#include "sequence/sequence_tools.hpp"

#include <string>
#include <memory>

#define MEM_F_SOFTCLIP  0x200

#define _set_pac(pac, l, c) ((pac)[(l)>>2] |= uint8_t((c)<<((~(l)&3)<<1)))
#define _get_pac(pac, l) ((pac)[(l)>>2]>>((~(l)&3)<<1)&3)
extern "C" {
int is_bwt(uint8_t *T, bwtint_t n);
};

namespace alignment {

BWAIndex::BWAIndex(const debruijn_graph::Graph& g, AlignmentMode mode)
        : g_(g),
          memopt_(mem_opt_init(), free),
          idx_(nullptr, bwa_idx_destroy),
          mode_(mode),
          skip_secondary_(true) {
    memopt_->flag |= MEM_F_SOFTCLIP;
    switch (mode) {
        default:
        case AlignmentMode::Default:
            break;
        case AlignmentMode::IntraCtg:
            memopt_->o_del = 16; memopt_->o_ins = 16;
            memopt_->b = 9;
            memopt_->pen_clip5 = 5; memopt_->pen_clip3 = 5;
            break;
        case AlignmentMode::PacBio:
        case AlignmentMode::Ont2D:
            memopt_->o_del = 1; memopt_->e_del = 1;
            memopt_->o_ins = 1; memopt_->e_ins = 1;
            memopt_->b = 1;
            memopt_->split_factor = 10.;
            memopt_->pen_clip5 = 0; memopt_->pen_clip3 = 0;
            memopt_->min_seed_len = 14;
            memopt_->mask_level = 20;
            memopt_->drop_ratio = 20;
            memopt_->min_chain_weight = 40;
            skip_secondary_ = false;
            break;
        case AlignmentMode::Protein:
            memopt_->o_del = 10000; memopt_->e_del = 10000;
            memopt_->o_ins = 10000; memopt_->e_ins = 10000;
            memopt_->b = 1;
            memopt_->split_factor = 10.;
            memopt_->pen_clip5 = 0; memopt_->pen_clip3 = 0;
            memopt_->min_seed_len = 7;
            memopt_->drop_ratio = 20;
            memopt_->mask_level = 20;
            memopt_->min_chain_weight = 10;
            break;
    };

    Init();
}

BWAIndex::~BWAIndex() {}

static uint8_t* seqlib_add1(const std::string &seq, const std::string &name,
                            bntseq_t *bns, uint8_t *pac, int64_t *m_pac, int *m_seqs, int *m_holes, bntamb1_t **q) {
    bntann1_t *p;
    int lasts;
    if (bns->n_seqs == *m_seqs) {
        *m_seqs <<= 1;
        bns->anns = (bntann1_t*)realloc(bns->anns, *m_seqs * sizeof(bntann1_t));
    }
    p = bns->anns + bns->n_seqs;
    p->name = strdup(name.c_str());
    p->anno = strdup("(null");
    p->gi = 0; p->len = int(seq.size());
    p->offset = (bns->n_seqs == 0)? 0 : (p-1)->offset + (p-1)->len;
    p->n_ambs = 0;
    for (size_t i = lasts = 0; i < seq.size(); ++i) {
        int c = nst_nt4_table[unsigned(seq[i])];
        if (c >= 4) { // N
            if (lasts == seq[i]) { // contiguous N
                ++(*q)->len;
            } else {
                if (bns->n_holes == *m_holes) {
                    (*m_holes) <<= 1;
                    bns->ambs = (bntamb1_t*)realloc(bns->ambs, (*m_holes) * sizeof(bntamb1_t));
                }
                *q = bns->ambs + bns->n_holes;
                (*q)->len = 1;
                (*q)->offset = p->offset + i;
                (*q)->amb = seq[i];
                ++p->n_ambs;
                ++bns->n_holes;
            }
        }
        lasts = seq[i];
        { // fill buffer
            if (c >= 4) c = lrand48() & 3;
            if (bns->l_pac == *m_pac) { // double the pac size
                *m_pac <<= 1;
                pac = (uint8_t*)realloc(pac, *m_pac/4);
                memset(pac + bns->l_pac/4, 0, (*m_pac - bns->l_pac)/4);
            }
            _set_pac(pac, bns->l_pac, c);
            ++bns->l_pac;
        }
    }
    ++bns->n_seqs;

    return pac;
}

static uint8_t* seqlib_make_pac(const debruijn_graph::Graph &g,
                                const std::vector<debruijn_graph::EdgeId> &ids,
                                BWAIndex::AlignmentMode mode,
                                bool for_only) {
    bntseq_t * bns = (bntseq_t*)calloc(1, sizeof(bntseq_t));
    uint8_t *pac = 0;
    int32_t m_seqs, m_holes;
    int64_t m_pac, l;
    bntamb1_t *q;

    bns->seed = 11; // fixed seed for random generator
    m_seqs = m_holes = 8; m_pac = 0x10000;
    bns->anns = (bntann1_t*)calloc(m_seqs, sizeof(bntann1_t));
    bns->ambs = (bntamb1_t*)calloc(m_holes, sizeof(bntamb1_t));
    pac = (uint8_t*) calloc(m_pac/4, 1);
    q = bns->ambs;

    // Move through the sequences
    int cur_len = 0;
    for (auto e : ids) {
        std::string ref = std::to_string(g.int_id(e));
        if (mode == BWAIndex::AlignmentMode::Protein) {
            std::string seq = g.EdgeNucls(e).str();
            for (size_t i = 0; i < 3; ++ i) {
                std::string cur_seq = ConvertNuc2CanonicalNuc(seq.substr(i));
                cur_len += cur_seq.size();
                pac = seqlib_add1(cur_seq, ref + "_" + std::to_string(i), bns, pac, &m_pac, &m_seqs, &m_holes, &q);
            }
        } else {
            std::string seq = g.EdgeNucls(e).str();
            // make the forward only pac
            cur_len += seq.size();
            pac = seqlib_add1(seq, ref, bns, pac, &m_pac, &m_seqs, &m_holes, &q);
        }
    }

    if (!for_only) {
        // add the reverse complemented sequence
        m_pac = (bns->l_pac * 2 + 3) / 4 * 4;
        pac = (uint8_t*)realloc(pac, m_pac/4);
        memset(pac + (bns->l_pac+3)/4, 0, (m_pac - (bns->l_pac+3)/4*4) / 4);
        for (l = bns->l_pac - 1; l >= 0; --l, ++bns->l_pac)
            _set_pac(pac, bns->l_pac, 3-_get_pac(pac, l));
    }

    bns_destroy(bns);

    return pac;
}

static bwt_t *seqlib_bwt_pac2bwt(const uint8_t *pac, size_t bwt_seq_lenr) {
    bwt_t *bwt;
    ubyte_t *buf;

    // Initialization
    bwt = (bwt_t*)calloc(1, sizeof(bwt_t));
    bwt->seq_len = bwt_seq_lenr;
    bwt->bwt_size = (bwt->seq_len + 15) >> 4;

    // Prepare sequence
    memset(bwt->L2, 0, 5 * 4);
    buf = (ubyte_t*)calloc(bwt->seq_len + 1, 1);
    for (bwtint_t i = 0; i < bwt->seq_len; ++i) {
        buf[i] = pac[i>>2] >> ((3 - (i&3)) << 1) & 3;
        ++bwt->L2[1+buf[i]];
    }
    for (bwtint_t i = 2; i <= 4; ++i)
        bwt->L2[i] += bwt->L2[i-1];

    // Burrows-Wheeler Transform
    if (bwt_seq_lenr < 50000000) {
        INFO("Using BWA IS algorithm");
        bwt->primary = is_bwt(buf, bwt->seq_len);
    } else {
        INFO("Using BWA RopeBWT algorithm");
        rope_t *r;
        int64_t x, i;
        rpitr_t itr;
        const uint8_t *blk;

        r = rope_init(ROPE_DEF_MAX_NODES, ROPE_DEF_BLOCK_LEN);
        for (i = bwt->seq_len - 1, x = 0; i >= 0; --i) {
            int c = buf[i] + 1;
            x = rope_insert_run(r, x, c, 1, 0) + 1;
            while (--c >= 0) x += r->c[c];
        }
        bwt->primary = x;
        rope_itr_first(r, &itr);
        x = 0;
        while ((blk = rope_itr_next_block(&itr)) != 0) {
            const uint8_t *q = blk + 2, *end = blk + 2 + *rle_nptr(blk);
            while (q < end) {
                int c = 0;
                int64_t l;
                rle_dec1(q, c, l);
                for (i = 0; i < l; ++i)
                    buf[x++] = ubyte_t(c - 1);
            }
        }
        rope_destroy(r);
    }
    bwt->bwt = (uint32_t*)calloc(bwt->bwt_size, 4);
    for (bwtint_t  i = 0; i < bwt->seq_len; ++i)
        bwt->bwt[i>>4] |= buf[i] << ((15 - (i&15)) << 1);
    free(buf);
    return bwt;
}

static bntann1_t* seqlib_add_to_anns(const std::string& name, const std::string& seq, bntann1_t* ann, size_t offset) {
    ann->offset = offset;
    ann->name = strdup(name.c_str());
    ann->anno = strdup("(null)");
    ann->len = int(seq.length());
    ann->n_ambs = 0; // number of "holes"
    ann->gi = 0; // gi?
    ann->is_alt = 0;

    return ann;
}

void BWAIndex::Init() {
    idx_.reset((bwaidx_t*)calloc(1, sizeof(bwaidx_t)));
    ids_.clear();
    bool no_conjugate = true;
    if (mode_ == AlignmentMode::Protein) {
        no_conjugate = false;
    }

    for (auto it = g_.ConstEdgeBegin(no_conjugate); !it.IsEnd(); ++it) {
        ids_.push_back(*it);
    }

    // construct the forward-only pac
    uint8_t* fwd_pac = seqlib_make_pac(g_, ids_, mode_, true); // true->for_only

    // construct the forward-reverse pac ("packed" 2 bit sequence)
    uint8_t* pac = seqlib_make_pac(g_, ids_, mode_, false); // don't write, because only used to make BWT

    size_t tlen = 0;
    if (mode_ == AlignmentMode::Protein) {
        for (auto e : ids_) {
            size_t sz = g_.EdgeNucls(e).size();
            for (size_t i = 0; i < 3; ++ i) {
                size_t protein_sz = (sz - i)/3;
                tlen += 3 * protein_sz;
            }
        }
    }else {
        for (auto e : ids_)
            tlen += g_.EdgeNucls(e).size();
    }

    // make the bwt
    bwt_t *bwt;
    bwt = seqlib_bwt_pac2bwt(pac, tlen*2); // *2 for fwd and rev
    bwt_bwtupdate_core(bwt);
    free(pac); // done with fwd-rev pac

    // construct sa from bwt and occ. adds it to bwt struct
    bwt_cal_sa(bwt, 32);
    bwt_gen_cnt_table(bwt);

    // make the bns
    bntseq_t * bns = (bntseq_t*) calloc(1, sizeof(bntseq_t));
    bns->l_pac = tlen;
    if (mode_ == AlignmentMode::Protein) {
        bns->n_seqs = int(3*ids_.size());
    } else {
        bns->n_seqs = int(ids_.size());
    }
    bns->seed = 11;
    bns->n_holes = 0;

    // make the anns
    // FIXME: Do we really need this?
    if (mode_ == AlignmentMode::Protein) {
        bns->anns = (bntann1_t*)calloc(3*ids_.size(), sizeof(bntann1_t));
    } else {
        bns->anns = (bntann1_t*)calloc(ids_.size(), sizeof(bntann1_t));
    }
    size_t offset = 0, k = 0;
    for (auto e: ids_) {
        std::string name = std::to_string(g_.int_id(e));
        if (mode_ == AlignmentMode::Protein) {
            std::string seq = g_.EdgeNucls(e).str();
            for (size_t i = 0; i < 3; ++ i) {
                std::string cur_seq = ConvertNuc2CanonicalNuc(seq.substr(i));
                seqlib_add_to_anns(name + "_" + std::to_string(i), cur_seq, &bns->anns[k++], offset);
                offset += cur_seq.length();
            }
        } else {
            std::string seq = g_.EdgeNucls(e).str();
            seqlib_add_to_anns(name, seq, &bns->anns[k++], offset);
            offset += seq.length();
        }
    }

    // ambs is "holes", like N bases
    bns->ambs = 0;

    // Make the in-memory idx struct
    idx_->bwt = bwt;
    idx_->bns = bns;
    idx_->pac = fwd_pac;
}

#if 0
        fprintf(stderr, "%zu: [%lld, %lld)\t[%d, %d) %c %d %s %ld %d\n",
                i,
                pos, pos + a.re - a.rb, a.qb, a.qe,
                "+-"[is_rev], a.rid,
                idx_->bns->anns[a.rid].name, g_.int_id(ids_[a.rid]), a.secondary);
#endif

#if 0
        mem_aln_t aln = mem_reg2aln(memopt_.get(), idx_->bns, idx_->pac, seq.length(), seq.c_str(), &a);

        // print alignment
        fprintf(stderr, "\t%c\t%s\t%ld %ld %ld\t%d\t", "+-"[aln.is_rev], idx_->bns->anns[aln.rid].name, aln.rid, g_.int_id(ids_[aln.rid]), (long)aln.pos, aln.mapq);
        for (int k = 0; k < aln.n_cigar; ++k) // print CIGAR
            fprintf(stderr, "%d%c", aln.cigar[k]>>4, "MIDSH"[aln.cigar[k]&0xf]);
        fprintf(stderr, "\t%d\n", aln.NM); // print edit distance
        free(aln.cigar);
#endif

inline std::ostream& operator<<(std::ostream& os, const mem_alnreg_s& a) {
    os << a.qb << " - " << a.qe << " ---> " << a.rb << " - " << a.re << "query->ref\n";
    os << a.seedcov << " - seedcov; " << a.score << " - score";
    return os;
}

static bool MostlyInVertex(size_t rb, size_t re, size_t edge_len, size_t k) {
//  k-rb > re - k
    if (rb < k && 2 * k  > re + rb)
        return true;
//  re - edge_len > edge_len - rb
    if (re > edge_len && re + rb > 2 * edge_len)
        return true;
    return false;
}


inline void cut_interval(size_t &s_begin, size_t &s_end, size_t &e_begin, size_t &e_end){
    size_t d3 = s_begin % 3;
    s_begin += (3 - d3) % 3;
    e_begin += (3 - d3) % 3;
    d3 = s_end % 3;
    s_end -= d3;
    e_end -= d3;
}

omnigraph::MappingPath<debruijn_graph::EdgeId> BWAIndex::GetMappingPath(const mem_alnreg_v &ar, const std::string &seq) const {
    omnigraph::MappingPath<debruijn_graph::EdgeId> res;

    // Turn read length into k-mers
    bool is_short = false;
    size_t seq_len = seq.length();
    if (seq_len <= g_.k()) {
        is_short = true;
    }
    for (size_t i = 0; i < ar.n; ++i) {
        const mem_alnreg_t &a = ar.a[i];

        if (skip_secondary_ && a.secondary >= 0) continue; // skip secondary alignments

        if (AlignmentMode::Protein == mode_) {
            if (a.qe - a.qb != a.re - a.rb) {
                WARN("Strange: " << a.rb << " " << a.re << " " << a.qb << " " << a.qe)
                continue;
            }
        }

        if (is_short) {
// skipping alignments shorter than half of read length
            if (size_t(a.qe - a.qb) * 2 <= seq_len ) continue;
            if (size_t(a.re - a.rb) * 2 <= seq_len) continue;
        } else {
            size_t min_length = g_.k();
            if (AlignmentMode::Protein == mode_) {
                min_length = std::min(seq_len/2, g_.k());
            }
            if (size_t(a.qe - a.qb) <= min_length) continue; // skip short alignments
            if (size_t(a.re - a.rb) <= min_length) continue;
        }
        int is_rev = 0;
        size_t pos = bns_depos(idx_->bns, a.rb < idx_->bns->l_pac? a.rb : a.re - 1, &is_rev) - idx_->bns->anns[a.rid].offset;
        size_t initial_range_end;
        size_t mapping_range_end;

        size_t edge_index = a.rid;
        size_t offset = 0;
        if (mode_ == AlignmentMode::Protein) {
            edge_index /= 3;
            offset = a.rid % 3;
        }

        // Reduce the range to kmer-based
        pos += offset;
        if (AlignmentMode::Protein == mode_){
            initial_range_end = a.qe;
            mapping_range_end = pos + a.re - a.rb;
        } else {
            if (is_short) {
                initial_range_end = a.qb;
                mapping_range_end = pos;
                if (mapping_range_end > g_.length(ids_[edge_index]))
                    continue;
            } else {
                initial_range_end = a.qe - g_.k();
                mapping_range_end = pos + a.re - a.rb - g_.k();
            }
        }
        DEBUG(a);
//FIXME: what about other scoring systems?
        double qual = double(a.score)/double(a.qe - a.qb);
        DEBUG("Edge: "<< ids_[a.rid] << " quality from score: " << qual);
        //Important for alignments shorter than K
        if (MostlyInVertex(pos, pos + a.re - a.rb, g_.length(ids_[edge_index]), g_.k()))
            continue;

        size_t initial_range_start = a.qb;
        if (AlignmentMode::Protein == mode_){
            if ((pos - offset) % 3 != initial_range_start % 3) continue;
            cut_interval(initial_range_start, initial_range_end, pos, mapping_range_end);
        }

        if (!is_rev) {
            res.push_back(ids_[edge_index],
                          { { (size_t)initial_range_start, initial_range_end },
                            { pos, mapping_range_end }, qual});

        } else {
            if (AlignmentMode::Protein == mode_){
                res.push_back(g_.conjugate(ids_[edge_index]),
                              { { (size_t)initial_range_start, initial_range_end }, //.Invert(read_length),
                                Range(pos,  mapping_range_end ).Invert(g_.length(ids_[edge_index]) + g_.k() - 1) , qual});
            } else {
                res.push_back(g_.conjugate(ids_[edge_index]),
                              { { (size_t)initial_range_start, initial_range_end }, //.Invert(read_length),
                                Range(pos,  mapping_range_end ).Invert(g_.length(ids_[edge_index])) , qual});
            }

        }
    }
    return res;
}


omnigraph::MappingPath<debruijn_graph::EdgeId> BWAIndex::AlignSequence(const Sequence &sequence) const {
    omnigraph::MappingPath<debruijn_graph::EdgeId> res;
    VERIFY(idx_);

    std::string seq = sequence.str();
    mem_alnreg_v ar = mem_align1(memopt_.get(), idx_->bwt, idx_->bns, idx_->pac,
                                 int(seq.length()), seq.data());
    res = GetMappingPath(ar, seq);

    free(ar.a);

    return res;
}

}
