#ifndef GENOTYPE_H
#define GENOTYPE_H

#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/iostreams/filtering_streambuf.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <boost/iostreams/copy.hpp>
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/iostreams/device/file.hpp>
#include <boost/progress.hpp>

#include <htslib/sam.h>

#include "util.h"

namespace torali
{

  struct Geno {
    int32_t chr;
    int32_t regionStart;
    int32_t regionEnd;
    int32_t bppos;
    int32_t svt;
    double score;
    std::string ref;
    std::string alt;

    Geno() : chr(-1), regionStart(0), regionEnd(0), bppos(0), svt(-1), score(1) {}
  };
  

  template<typename TConfig>
  inline int32_t
  scoreThreshold(std::string const& s1, std::string const& s2, TConfig const& c) {
    int32_t l1 = 0;
    int32_t l2 = 0;
    for(uint32_t i = 0; i < s1.size(); ++i) {
      if (s1[i] != '-') ++l1;
    }
    for(uint32_t i = 0; i < s2.size(); ++i) {
      if (s2[i] != '-') ++l2;
    }
    int32_t minSize = std::min(l1, l2);
    return (int32_t) ((c.flankQuality * minSize * c.aliscore.match) + ((1.0 - c.flankQuality) * minSize * c.aliscore.mismatch));
  }
  
  template<typename TScore>
  inline int32_t
  scoreAlign(std::string const& s1, std::string const& s2, TScore const& sc) {
    int32_t score = 0;
    bool inGap = false;
    for(uint32_t i = 0; i < s1.size(); ++i) {
      if ((s1[i] == '-') || (s2[i] == '-')) {
	if (inGap) score += sc.ge;
	else {
	  inGap = true;
	  score += (sc.go + sc.ge);
	}
      } else {
	inGap = false;
	if (s1[i] == s2[i]) score += sc.match;
	else score += sc.mismatch;
      }
    }
    return score;
  }

  template<typename TConfig, typename TJunctionMap, typename TReadCountMap>
  inline void
  trackRef(TConfig& c, std::vector<StructuralVariantRecord>& svs, TJunctionMap& jctMap, TReadCountMap& covMap) {
    typedef std::vector<StructuralVariantRecord> TSVs;
    typedef std::vector<uint8_t> TQuality;
    typedef boost::multi_array<char, 2> TAlign;
    AlignConfig<false, false> global;
    if (svs.empty()) return;

    // Open file handles
    typedef std::vector<samFile*> TSamFile;
    typedef std::vector<hts_idx_t*> TIndex;
    typedef std::vector<bam_hdr_t*> THeader;
    TSamFile samfile(c.files.size());
    TIndex idx(c.files.size());
    THeader hdr(c.files.size());
    int32_t totalTarget = 0;
    for(uint32_t file_c = 0; file_c < c.files.size(); ++file_c) {
      samfile[file_c] = sam_open(c.files[file_c].string().c_str(), "r");
      hts_set_fai_filename(samfile[file_c], c.genome.string().c_str());
      idx[file_c] = sam_index_load(samfile[file_c], c.files[file_c].string().c_str());
      hdr[file_c] = sam_hdr_read(samfile[file_c]);
      totalTarget += hdr[file_c]->n_targets;
    }

    // Reference and consensus probes
    typedef std::vector<Geno> TGenoRegion;
    TGenoRegion gbp(svs.size(), Geno());

    // Preprocess REF and ALT
    boost::posix_time::ptime noww = boost::posix_time::second_clock::local_time();
    std::cout << '[' << boost::posix_time::to_simple_string(noww) << "] " << "Generate REF and ALT probes" << std::endl;
    boost::progress_display show_progresss( hdr[0]->n_targets );

    std::vector<std::string> refProbes(svs.size());
    faidx_t* fai = fai_load(c.genome.string().c_str());
    for(int32_t refIndex=0; refIndex < (int32_t) hdr[0]->n_targets; ++refIndex) {
      ++show_progresss;
      char* seq = NULL;

      // Iterate all structural variants
      for(typename TSVs::iterator itSV = svs.begin(); itSV != svs.end(); ++itSV) {
	if ((itSV->chr != refIndex) && (itSV->chr2 != refIndex)) continue;

	// Lazy loading of reference sequence
	if (seq == NULL) {
	  int32_t seqlen = -1;
	  std::string tname(hdr[0]->target_name[refIndex]);
	  seq = faidx_fetch_seq(fai, tname.c_str(), 0, hdr[0]->target_len[refIndex], &seqlen);
	}

	// Set tag alleles
	if (itSV->chr == refIndex) {
	  itSV->alleles = _addAlleles(boost::to_upper_copy(std::string(seq + itSV->svStart - 1, seq + itSV->svStart)), std::string(hdr[0]->target_name[itSV->chr2]), *itSV, itSV->svt);
	}
	if (!itSV->precise) continue;

	// Get the reference sequence
	if ((itSV->chr != itSV->chr2) && (itSV->chr2 == refIndex)) {
	  Breakpoint bp(*itSV);
	  _initBreakpoint(hdr[0], bp, (int32_t) itSV->consensus.size(), itSV->svt);
	  refProbes[itSV->id] = _getSVRef(seq, bp, refIndex, itSV->svt);
	}
	if (itSV->chr == refIndex) {
	  Breakpoint bp(*itSV);
	  if (_translocation(itSV->svt)) bp.part1 = refProbes[itSV->id];
	  if (itSV->svt ==4) {
	    int32_t bufferSpace = std::max((int32_t) ((itSV->consensus.size() - itSV->insLen) / 3), c.minimumFlankSize);
	    _initBreakpoint(hdr[0], bp, bufferSpace, itSV->svt);
	  } else _initBreakpoint(hdr[0], bp, (int32_t) itSV->consensus.size(), itSV->svt);
	  std::string svRefStr = _getSVRef(seq, bp, refIndex, itSV->svt);
	  
	  // Find breakpoint to reference
	  TAlign align;
	  if (!_consRefAlignment(itSV->consensus, svRefStr, align, itSV->svt)) continue;

	  AlignDescriptor ad;
	  if (!_findSplit(c, itSV->consensus, svRefStr, align, ad, itSV->svt)) continue;
	  
	  // Debug consensus to reference alignment
	  //std::cerr << itSV->id << std::endl;
	  //for(uint32_t i = 0; i<align.shape()[0]; ++i) {
	  //for(uint32_t j = 0; j<align.shape()[1]; ++j) std::cerr << align[i][j];
	  //std::cerr << std::endl;
	  //}
	  //std::cerr << std::endl;

	  // Find allele-tagging probes
	  int32_t stepSize = std::max(c.minimumFlankSize / 5, 2);
	  for(int32_t boundary = c.minimumFlankSize; boundary <= std::max(c.minimumFlankSize, (int32_t) itSV->consensus.size()); boundary += stepSize) {
	    if (c.minimumFlankSize + boundary >= (int32_t) itSV->consensus.size()) break;
	    int32_t leftCutConsStart = std::max(0, ad.cStart - c.minimumFlankSize);
	    int32_t leftCutConsEnd = std::min(ad.cStart + boundary, (int32_t) itSV->consensus.size());
	    int32_t leftSize = leftCutConsEnd - leftCutConsStart;
	    int32_t leftRegionStart = std::max(0, itSV->svStart - c.minimumFlankSize);
	    int32_t leftRegionEnd = std::min(leftRegionStart + leftSize, (int32_t) hdr[0]->target_len[itSV->chr]);
	    std::string consLeft;
	    std::string refLeft;
	    TAlign alignLeft;
	    double scoreLeft = 1;
	    if ((leftRegionStart < itSV->svStart) && (itSV->svStart < leftRegionEnd) && (leftRegionEnd - leftRegionStart <= (int32_t) itSV->consensus.size())) {
	      consLeft = itSV->consensus.substr(leftCutConsStart, leftSize);
	      refLeft = boost::to_upper_copy(std::string(seq + leftRegionStart, seq + leftRegionEnd));
	      scoreLeft = gotoh(consLeft, refLeft, alignLeft, global, c.aliscore);
	      scoreLeft /= (double) (consLeft.size() * c.aliscore.match);
	    }
	    TAlign alignRight;
	    double scoreRight = 1;
	    int32_t rightRegionStart = 0;
	    int32_t rightRegionEnd = 0;
	    std::string consRight;
	    std::string refRight;
	    if (itSV->chr == itSV->chr2) {
	      int32_t rightCutConsStart = std::max(0, ad.cEnd - boundary);
	      int32_t rightCutConsEnd = std::min(ad.cEnd + c.minimumFlankSize, (int32_t) itSV->consensus.size());
	      int32_t rightSize = rightCutConsEnd - rightCutConsStart;
	      rightRegionStart = std::max(0, itSV->svEnd - boundary);
	      rightRegionEnd = std::min(rightRegionStart + rightSize, (int32_t) hdr[0]->target_len[itSV->chr2]);
	      if ((rightRegionStart < itSV->svEnd) && (itSV->svEnd < rightRegionEnd) && (rightRegionEnd - rightRegionStart <= (int32_t) itSV->consensus.size())) {
		consRight = itSV->consensus.substr(rightCutConsStart, rightSize);
		refRight = boost::to_upper_copy(std::string(seq + rightRegionStart, seq + rightRegionEnd));
		scoreRight = gotoh(consRight, refRight, alignRight, global, c.aliscore);
		scoreRight /= (double) (consRight.size() * c.aliscore.match);
	      }
	    }
	    // Debug probe finding
	    //std::cerr << "SV:" << itSV->id << ",SVT:" << itSV->svt << ",Left:" << scoreLeft << ",Right:" << scoreRight << std::endl;
	    //for(uint32_t i = 0; i < alignLeft.shape()[0]; ++i) {
	    //for(uint32_t j = 0; j < alignLeft.shape()[1]; ++j) std::cerr << alignLeft[i][j];
	    //std::cerr << std::endl;
	    //}
	    //for(uint32_t i = 0; i < alignRight.shape()[0]; ++i) {
	    //for(uint32_t j = 0; j < alignRight.shape()[1]; ++j) std::cerr << alignRight[i][j];
	    //std::cerr << std::endl;
	    //}
	    if (scoreLeft < scoreRight) {
	      if (scoreLeft < gbp[itSV->id].score) {
		gbp[itSV->id].chr = itSV->chr;
		gbp[itSV->id].score = scoreLeft;
		gbp[itSV->id].regionStart = leftRegionStart;
		gbp[itSV->id].regionEnd = leftRegionEnd;
		gbp[itSV->id].bppos = itSV->svStart;
		gbp[itSV->id].ref = refLeft;
		gbp[itSV->id].alt = consLeft;
		gbp[itSV->id].svt = itSV->svt;
	      }
	    } else {
	      if (scoreRight < gbp[itSV->id].score) {
		gbp[itSV->id].chr = itSV->chr2;
		gbp[itSV->id].score = scoreRight;
		gbp[itSV->id].regionStart = rightRegionStart;
		gbp[itSV->id].regionEnd = rightRegionEnd;
		gbp[itSV->id].bppos = itSV->svEnd;
		gbp[itSV->id].ref = refRight;
		gbp[itSV->id].alt = consRight;
		gbp[itSV->id].svt = itSV->svt;
	      }
	    }
	    if (gbp[itSV->id].score < 0.8) break;
	  }
	}
      }
      if (seq != NULL) free(seq);
    }
    // Clean-up
    fai_destroy(fai);

    // Parse genome chr-by-chr
    boost::posix_time::ptime now = boost::posix_time::second_clock::local_time();
    std::cout << '[' << boost::posix_time::to_simple_string(now) << "] " << "SV annotation" << std::endl;
    boost::progress_display show_progress( totalTarget );

    typedef std::vector<uint32_t> TRefAlignCount;
    typedef std::vector<TRefAlignCount> TFileRefAlignCount;
    TFileRefAlignCount refAlignedReadCount(c.files.size(), TRefAlignCount());
    for(unsigned int file_c = 0; file_c < c.files.size(); ++file_c) refAlignedReadCount[file_c].resize(svs.size(), 0);

    // Coverage distribution
    typedef uint16_t TMaxCoverage;
    uint32_t maxCoverage = std::numeric_limits<TMaxCoverage>::max();
    typedef std::vector<uint32_t> TCovDist;
    typedef std::vector<TCovDist> TSampleCovDist;
    TSampleCovDist covDist(c.files.size(), TCovDist());
    for(uint32_t i = 0; i < c.files.size(); ++i) covDist[i].resize(maxCoverage, 0);

    // Error rates
    std::vector<uint64_t> matchCount(c.files.size(), 0);
    std::vector<uint64_t> mismatchCount(c.files.size(), 0);
    std::vector<uint64_t> delCount(c.files.size(), 0);
    std::vector<uint64_t> insCount(c.files.size(), 0);

    // Read length distribution
    typedef uint16_t TMaxReadLength;
    uint32_t maxReadLength = std::numeric_limits<TMaxReadLength>::max();
    uint32_t rlBinSize = 100;
    typedef std::vector<uint32_t> TReadLengthDist;
    typedef std::vector<TReadLengthDist> TSampleRLDist;
    TSampleRLDist rlDist(c.files.size(), TReadLengthDist());
    for(uint32_t i = 0; i < c.files.size(); ++i) rlDist[i].resize(maxReadLength * rlBinSize, 0);

    // Dump file
    boost::iostreams::filtering_ostream dumpOut;
    if (c.hasDumpFile) {
      dumpOut.push(boost::iostreams::gzip_compressor());
      dumpOut.push(boost::iostreams::file_sink(c.dumpfile.string().c_str(), std::ios_base::out | std::ios_base::binary));
      dumpOut << "#svid\tbam\tqname\tchr\tpos\tmatechr\tmatepos\tmapq\ttype" << std::endl;
    }

    // Iterate samples
    for(unsigned int file_c = 0; file_c < c.files.size(); ++file_c) {
      // Iterate chromosomes
      for(int32_t refIndex=0; refIndex < (int32_t) hdr[file_c]->n_targets; ++refIndex) {
	++show_progress;

	// Check we have mapped reads on this chromosome
	bool nodata = true;
	std::string suffix("cram");
	std::string str(c.files[file_c].string());
	if ((str.size() >= suffix.size()) && (str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0)) nodata = false;
	uint64_t mapped = 0;
	uint64_t unmapped = 0;
	hts_idx_get_stat(idx[file_c], refIndex, &mapped, &unmapped);
	if (mapped) nodata = false;
	if (nodata) continue;

	// Coverage track
	typedef std::vector<TMaxCoverage> TBpCoverage;
	TBpCoverage covBases(hdr[file_c]->target_len[refIndex], 0);

	// Flag breakpoints
	typedef std::set<int32_t> TIdSet;
	typedef std::map<uint32_t, TIdSet> TBpToIdMap;
	TBpToIdMap bpid;
	typedef boost::dynamic_bitset<> TBitSet;
	TBitSet bpOccupied(hdr[file_c]->target_len[refIndex], false);
	for(uint32_t i = 0; i < gbp.size(); ++i) {
	  if (gbp[i].chr == refIndex) {
	    bpOccupied[gbp[i].bppos] = 1;
	    if (bpid.find(gbp[i].bppos) == bpid.end()) bpid.insert(std::make_pair(gbp[i].bppos, TIdSet()));
	    bpid[gbp[i].bppos].insert(i);
	  }
	}

	// Count reads
	hts_itr_t* iter = sam_itr_queryi(idx[file_c], refIndex, 0, hdr[file_c]->target_len[refIndex]);
	bam1_t* rec = bam_init1();
	while (sam_itr_next(samfile[file_c], iter, rec) >= 0) {
	  // Genotyping only primary alignments
	  if (rec->core.flag & (BAM_FSECONDARY | BAM_FQCFAIL | BAM_FDUP | BAM_FSUPPLEMENTARY | BAM_FUNMAP)) continue;
	    
	  // Get sequence
	  std::string sequence;
	  sequence.resize(rec->core.l_qseq);
	  uint8_t* seqptr = bam_get_seq(rec);
	  for (int i = 0; i < rec->core.l_qseq; ++i) sequence[i] = "=ACMGRSVTWYHKDBN"[bam_seqi(seqptr, i)];

	  // Read length
	  uint32_t slen = rec->core.l_qseq;
	  if (!slen) slen = sequenceLength(rec);
	  if (slen < maxReadLength * rlBinSize) ++rlDist[file_c][(int32_t) (slen / rlBinSize)];

	  // Reference and sequence pointer
	  uint32_t rp = rec->core.pos; // reference pointer
	  uint32_t sp = 0; // sequence pointer
      
	  // Parse the CIGAR
	  uint32_t* cigar = bam_get_cigar(rec);
	  for (std::size_t i = 0; i < rec->core.n_cigar; ++i) {
	    if ((bam_cigar_op(cigar[i]) == BAM_CMATCH) || (bam_cigar_op(cigar[i]) == BAM_CEQUAL) || (bam_cigar_op(cigar[i]) == BAM_CDIFF)) {
	      // Fetch reference alignments
	      int32_t bpHit = 0;
	      int32_t spHit = 0;
	      // Preceeding SV breakpoint?
	      for(uint32_t k = std::max(0, (int32_t) rp - (int32_t) c.minRefSep); k < rp; ++k) {
		if (bpOccupied[k]) {
		  bpHit = k;
		  spHit = sp - (rp - k);
		}
	      }
	      // Hitting SV breakpoint?
	      for(uint32_t k = 0; k < bam_cigar_oplen(cigar[i]); ++k) {
		if ((rp < hdr[file_c]->target_len[refIndex]) && (covBases[rp] < maxCoverage - 1)) ++covBases[rp];
		if (bpOccupied[rp]) {
		  bpHit = rp;
		  spHit = sp;
		}
		if ((bam_cigar_op(cigar[i]) == BAM_CMATCH) || (bam_cigar_op(cigar[i]) == BAM_CEQUAL)) ++matchCount[file_c];
		else if (bam_cigar_op(cigar[i]) == BAM_CDIFF) ++mismatchCount[file_c];
		++sp;
		++rp;
	      }
	      // Succeeding SV breakpoint
	      for(uint32_t k = rp; k < std::min(rp + c.minRefSep, hdr[file_c]->target_len[refIndex]); ++k) {
		if (bpOccupied[k]) {
		  bpHit = k;
		  spHit = sp + (k - rp);
		}
	      }
	      if (bpHit) {
		// Genotype read with respect to all SVs at this position
		for(typename TIdSet::const_iterator it = bpid[bpHit].begin(); it != bpid[bpHit].end(); ++it) {
		  //std::cerr << "GBP:" << gbp[*it].regionStart << ',' << gbp[*it].bppos << ',' << gbp[*it].regionEnd << std::endl;
		  //std::cerr << spHit << ':' << sequence.size() << std::endl;

		  // Require spanning reads
		  int32_t leftOffset = gbp[*it].bppos - gbp[*it].regionStart;
		  if (spHit < leftOffset) continue;
		  int32_t rightOffset = gbp[*it].regionEnd - gbp[*it].bppos;
		  if ((int32_t) sequence.size() < rightOffset + spHit) continue;
		  std::string subseq = sequence.substr(spHit - leftOffset, leftOffset + rightOffset);

		  // Compute alignment to alternative haplotype
		  TAlign alignAlt;
		  int32_t scoreA = gotoh(gbp[*it].alt, subseq, alignAlt, global, c.aliscore);
		  int32_t scoreAltThreshold = (int32_t) (c.flankQuality * gbp[*it].alt.size() * c.aliscore.match + (1.0 - c.flankQuality) * gbp[*it].ref.size() * c.aliscore.mismatch);
		  double scoreAlt = (double) scoreA / (double) scoreAltThreshold;
		  
		  // Compute alignment to reference haplotype
		  TAlign alignRef;
		  int32_t scoreR = gotoh(gbp[*it].ref, subseq, alignRef, global, c.aliscore);
		  int32_t scoreRefThreshold = (int32_t) (c.flankQuality * gbp[*it].ref.size() * c.aliscore.match + (1.0 - c.flankQuality) * gbp[*it].ref.size() * c.aliscore.mismatch);
		  double scoreRef = (double) scoreR / (double) scoreRefThreshold;
		  
		  // Any confident alignment?
		  if ((scoreRef > 1) || (scoreAlt > 1)) {
		    // Debug alignment to REF and ALT
		    /*
		    std::cerr << "Alt:\t" << scoreAlt << "\tRef:\t" << scoreRef << std::endl;
		    for(uint32_t i = 0; i< alignAlt.shape()[0]; ++i) {
		      for(uint32_t j = 0; j< alignAlt.shape()[1]; ++j) std::cerr << alignAlt[i][j];
		      std::cerr << std::endl;
		    }
		    for(uint32_t i = 0; i< alignRef.shape()[0]; ++i) {
		      for(uint32_t j = 0; j< alignRef.shape()[1]; ++j) std::cerr << alignRef[i][j];
		      std::cerr << std::endl;
		    }
		    */
		    
		    if (scoreRef > scoreAlt) {
		      // Account for reference bias
		      if (++refAlignedReadCount[file_c][*it] % 2) {
			TQuality quality;
			quality.resize(rec->core.l_qseq);
			uint8_t* qualptr = bam_get_qual(rec);
			for (int i = 0; i < rec->core.l_qseq; ++i) quality[i] = qualptr[i];
			uint32_t rq = _getAlignmentQual(alignRef, quality);
			if (rq >= c.minGenoQual) {
			  uint8_t* hpptr = bam_aux_get(rec, "HP");
			  jctMap[file_c][*it].ref.push_back((uint8_t) std::min(rq, (uint32_t) rec->core.qual));
			  if (hpptr) {
			    c.isHaplotagged = true;
			    int hap = bam_aux2i(hpptr);
			    if (hap == 1) ++jctMap[file_c][*it].refh1;
			    else ++jctMap[file_c][*it].refh2;
			  }
			}
		      }
		    } else {
		      TQuality quality;
		      quality.resize(rec->core.l_qseq);
		      uint8_t* qualptr = bam_get_qual(rec);
		      for (int i = 0; i < rec->core.l_qseq; ++i) quality[i] = qualptr[i];
		      uint32_t aq = _getAlignmentQual(alignAlt, quality);
		      if (aq >= c.minGenoQual) {
			uint8_t* hpptr = bam_aux_get(rec, "HP");
			if (c.hasDumpFile) {
			  std::string svid(_addID(gbp[*it].svt));
			  std::string padNumber = boost::lexical_cast<std::string>(*it);
			  padNumber.insert(padNumber.begin(), 8 - padNumber.length(), '0');
			  svid += padNumber;
			  dumpOut << svid << "\t" << c.files[file_c].string() << "\t" << bam_get_qname(rec) << "\t" << hdr[file_c]->target_name[rec->core.tid] << "\t" << rec->core.pos << "\t" << hdr[file_c]->target_name[rec->core.mtid] << "\t" << rec->core.mpos << "\t" << (int32_t) rec->core.qual << "\tSR" << std::endl;
			}
			jctMap[file_c][*it].alt.push_back((uint8_t) std::min(aq, (uint32_t) rec->core.qual));
			if (hpptr) {
			  c.isHaplotagged = true;
			  int hap = bam_aux2i(hpptr);
			  if (hap == 1) ++jctMap[file_c][*it].alth1;
			  else ++jctMap[file_c][*it].alth2;
			}
		      }
		    }
		  }
		}
	      }
	    } else if ((bam_cigar_op(cigar[i]) == BAM_CDEL) || (bam_cigar_op(cigar[i]) == BAM_CREF_SKIP)) {
	      ++delCount[file_c];
	      rp += bam_cigar_oplen(cigar[i]);
	    } else if (bam_cigar_op(cigar[i]) == BAM_CINS) {
	      ++insCount[file_c];
	      sp += bam_cigar_oplen(cigar[i]);
	    } else if (bam_cigar_op(cigar[i]) == BAM_CSOFT_CLIP) {
	      sp += bam_cigar_oplen(cigar[i]);
	    } else if (bam_cigar_op(cigar[i]) == BAM_CHARD_CLIP) {
	      // Do nothing
	    } else {
	      std::cerr << "Unknown Cigar options" << std::endl;
	    }
	  }
	}
	// Clean-up
	bam_destroy1(rec);
	hts_itr_destroy(iter);
      
	// Summarize coverage for this chromosome
	for(uint32_t i = 0; i < hdr[file_c]->target_len[refIndex]; ++i) ++covDist[file_c][covBases[i]];
            
	// Assign SV support
	for(uint32_t i = 0; i < svs.size(); ++i) {
	  if (svs[i].chr == refIndex) {
	    int32_t halfSize = (svs[i].svEnd - svs[i].svStart)/2;
	    if ((_translocation(svs[i].svt)) || (svs[i].svt == 4)) halfSize = 500;

	    // Left region
	    int32_t lstart = std::max(svs[i].svStart - halfSize, 0);
	    int32_t lend = svs[i].svStart;
	    int32_t covbase = 0;
	    for(uint32_t k = lstart; ((k < (uint32_t) lend) && (k < hdr[file_c]->target_len[refIndex])); ++k) covbase += covBases[k];
	    covMap[file_c][svs[i].id].leftRC = covbase;

	    // Actual SV
	    covbase = 0;
	    int32_t mstart = svs[i].svStart;
	    int32_t mend = svs[i].svEnd;
	    if ((_translocation(svs[i].svt)) || (svs[i].svt == 4)) {
	      mstart = std::max(svs[i].svStart - halfSize, 0);
	      mend = std::min(svs[i].svStart + halfSize, (int32_t) hdr[file_c]->target_len[refIndex]);
	    }
	    for(uint32_t k = mstart; ((k < (uint32_t) mend) && (k < hdr[file_c]->target_len[refIndex])); ++k) covbase += covBases[k];
	    covMap[file_c][svs[i].id].rc = covbase;

	    // Right region
	    covbase = 0;
	    int32_t rstart = svs[i].svEnd;
	    int32_t rend = std::min(svs[i].svEnd + halfSize, (int32_t) hdr[file_c]->target_len[refIndex]);
	    if ((_translocation(svs[i].svt)) || (svs[i].svt == 4)) {
	      rstart = svs[i].svStart;
	      rend = std::min(svs[i].svStart + halfSize, (int32_t) hdr[file_c]->target_len[refIndex]);
	    }
	    for(uint32_t k = rstart; ((k < (uint32_t) rend) && (k < hdr[file_c]->target_len[refIndex])); ++k) covbase += covBases[k];
	    covMap[file_c][svs[i].id].rightRC = covbase;
	  }
	}
      }
    }

    // Output coverage info
    std::cout << "Coverage distribution (^COV)" << std::endl;
    for(uint32_t file_c = 0; file_c < c.files.size(); ++file_c) {
      uint64_t totalCovCount = 0;
      for (uint32_t i = 0; i < covDist[file_c].size(); ++i) totalCovCount += covDist[file_c][i];
      std::vector<uint32_t> covPercentiles(5, 0);  // 5%, 25%, 50%, 75%, 95%
      uint64_t cumCovCount = 0;
      for (uint32_t i = 0; i < covDist[file_c].size(); ++i) {
	cumCovCount += covDist[file_c][i];
	double frac = (double) cumCovCount / (double) totalCovCount;
	if (frac < 0.05) covPercentiles[0] = i + 1;
	if (frac < 0.25) covPercentiles[1] = i + 1;
	if (frac < 0.5) covPercentiles[2] = i + 1;
	if (frac < 0.75) covPercentiles[3] = i + 1;
	if (frac < 0.95) covPercentiles[4] = i + 1;
      }
      std::cout << "COV\t" << c.sampleName[file_c] << "\t95% of bases are >= " << covPercentiles[0] << "x" << std::endl;
      std::cout << "COV\t" << c.sampleName[file_c] << "\t75% of bases are >= " << covPercentiles[1] << "x" << std::endl;
      std::cout << "COV\t" << c.sampleName[file_c] << "\t50% of bases are >= " << covPercentiles[2] << "x" << std::endl;
      std::cout << "COV\t" << c.sampleName[file_c] << "\t25% of bases are >= " << covPercentiles[3] << "x" << std::endl;
      std::cout << "COV\t" << c.sampleName[file_c] << "\t5% of bases are >= " << covPercentiles[4] << "x" << std::endl;
    }
    
    // Output read length info
    std::cout << "Read-length distribution (^RL)" << std::endl;
    for(uint32_t file_c = 0; file_c < c.files.size(); ++file_c) {
      uint64_t totalRlCount = 0;
      for (uint32_t i = 0; i < rlDist[file_c].size(); ++i) totalRlCount += rlDist[file_c][i];
      std::vector<uint32_t> rlPercentiles(5, 0);  // 5%, 25%, 50%, 75%, 95%
      uint64_t cumRlCount = 0;
      for (uint32_t i = 0; i < rlDist[file_c].size(); ++i) {
	cumRlCount += rlDist[file_c][i];
	double frac = (double) cumRlCount / (double) totalRlCount;
	if (frac < 0.05) rlPercentiles[0] = (i + 1) * rlBinSize;
	if (frac < 0.25) rlPercentiles[1] = (i + 1) * rlBinSize;
	if (frac < 0.5) rlPercentiles[2] = (i + 1) * rlBinSize;
	if (frac < 0.75) rlPercentiles[3] = (i + 1) * rlBinSize;
	if (frac < 0.95) rlPercentiles[4] = (i + 1) * rlBinSize;
      }
      std::cout << "RL\t" << c.sampleName[file_c] << "\t95% of reads are >= " << rlPercentiles[0] << "bp" << std::endl;
      std::cout << "RL\t" << c.sampleName[file_c] << "\t75% of reads are >= " << rlPercentiles[1] << "bp" << std::endl;
      std::cout << "RL\t" << c.sampleName[file_c] << "\t50% of reads are >= " << rlPercentiles[2] << "bp" << std::endl;
      std::cout << "RL\t" << c.sampleName[file_c] << "\t25% of reads are >= " << rlPercentiles[3] << "bp" << std::endl;
      std::cout << "RL\t" << c.sampleName[file_c] << "\t5% of reads are >= " << rlPercentiles[4] << "bp" << std::endl;
    }
    
    // Output sequencing error rates
    std::cout << "Sequencing error rates (^ERR)" << std::endl;
    for(uint32_t file_c = 0; file_c < c.files.size(); ++file_c) {
      uint64_t alignedbases = matchCount[file_c] + mismatchCount[file_c] + delCount[file_c] + insCount[file_c];
      if (mismatchCount[file_c]) {
	std::cout << "ERR\t" << c.sampleName[file_c] << "\tMatchRate\t" << (double) matchCount[file_c] / (double) alignedbases << std::endl;
	std::cout << "ERR\t" << c.sampleName[file_c] << "\tMismatchRate\t" << (double) mismatchCount[file_c] / (double) alignedbases << std::endl;
      }
      std::cout << "ERR\t" << c.sampleName[file_c] << "\tDeletionRate\t" << (double) delCount[file_c] / (double) alignedbases << std::endl;
      std::cout << "ERR\t" << c.sampleName[file_c] << "\tInsertionRate\t" << (double) insCount[file_c] / (double) alignedbases << std::endl;
    }

    // Clean-up
    for(unsigned int file_c = 0; file_c < c.files.size(); ++file_c) {
      bam_hdr_destroy(hdr[file_c]);	  
      hts_idx_destroy(idx[file_c]);
      sam_close(samfile[file_c]);
    }
  }
     
  
}

#endif