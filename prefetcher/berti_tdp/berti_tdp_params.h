# ifndef _BERTI_TDP_PARAMETERS_H_
# define _BERTI_TDP_PARAMETERS_H_

namespace berti_tdp_space
{
  /*
   * Berti: an Accurate Local-Delta Data Prefetcher
   *  
   * 55th ACM/IEEE International Conference on Microarchitecture (MICRO 2022),
   * October 1-5, 2022, Chicago, Illinois, USA.
   * 
   * @Authors: Agustín Navarro-Torres, Biswabandan Panda, J. Alastruey-Benedé, 
   *           Pablo Ibáñez, Víctor Viñals-Yúfera, and Alberto Ros
   * @Manteiners: Agustín Navarro -Torres
   * @Email: agusnt@unizar.es
   * @Date: 22/11/2022
   * 
   * This code is an update from the original code to work with the new version
   * of ChampSim: https://github.com/agusnt/Berti-Artifact
   * 
   * Maybe fine-tuning is required to get the optimal performance/accuracy.
   * 
   * Please note that this version of ChampSim has noticeable differences with 
   * the used for the paper, so results can varies.
   * 
   * Cite this:
   * 
   * A. Navarro-Torres, B. Panda, J. Alastruey-Benedé, P. Ibáñez, 
   * V. Viñals-Yúfera and A. Ros, 
   * "Berti: an Accurate Local-Delta Data Prefetcher," 
   * 2022 55th IEEE/ACM International Symposium on Microarchitecture (MICRO), 
   * 2022, pp. 975-991, doi: 10.1109/MICRO56248.2022.00072.
   * 
   * @INPROCEEDINGS{9923806,  author={Navarro-Torres, Agustín and Panda, 
   * Biswabandan and Alastruey-Benedé, Jesús and Ibáñez, Pablo and Viñals-Yúfera,
   * Víctor and Ros, Alberto},  booktitle={2022 55th IEEE/ACM International 
   * Symposium on Microarchitecture (MICRO)},   title={Berti: an Accurate 
   * Local-Delta Data Prefetcher},   year={2022},  volume={},  number={},  
   * pages={975-991},  doi={10.1109/MICRO56248.2022.00072}}
   */
  
  /*****************************************************************************
   *                              SIZES                                        *
   *****************************************************************************/
  // BERTI
  # define BERTI_TABLE_SIZE             (64) // Big enough to fit all entries
  # define BERTI_TABLE_DELTA_SIZE       (16)
  
  // HISTORY
  # define HISTORY_TABLE_SETS           (8)
  # define HISTORY_TABLE_WAYS           (16)

  // Hash Function
  //#define HASH_FN
  //# define HASH_ORIGINAL
  //# define THOMAS_WANG_HASH_1
  //# define THOMAS_WANG_HASH_2
  //# define THOMAS_WANG_HASH_3
  //# define THOMAS_WANG_HASH_4
  //# define THOMAS_WANG_HASH_5
  //# define THOMAS_WANG_HASH_6
  //# define THOMAS_WANG_HASH_7
  //# define THOMAS_WANG_NEW_HASH
  //# define THOMAS_WANG_HASH_HALF_AVALANCHE
  //# define THOMAS_WANG_HASH_FULL_AVALANCHE
  //# define THOMAS_WANG_HASH_INT_1
  //# define THOMAS_WANG_HASH_INT_2
  # define ENTANGLING_HASH
  //# define FOLD_HASH
 
  /*****************************************************************************
   *                              MASKS                                        *
   *****************************************************************************/
  # define SIZE_IP_MASK (64)
  # define IP_MASK                      (0xFFFF)
  # define TIME_MASK                    (0xFFFF)
  # define LAT_MASK                     (0xFFF)
  # define ADDR_MASK                    (0xFFFFFF)
  # define DELTA_MASK                   (12)
  # define TABLE_SET_MASK               (0x7)
  
  /*****************************************************************************
   *                      CONFIDENCE VALUES                                    *
   *****************************************************************************/
  # define CONFIDENCE_MAX               (16) // 6 bits
  # define CONFIDENCE_INC               (1) // 6 bits
  # define CONFIDENCE_INIT              (1) // 6 bits
   
  # define CONFIDENCE_L1                (10) // 6 bits
  # define CONFIDENCE_L2                (8) // 6 bits
  # define CONFIDENCE_L2R               (6) // 6 bits
  
  # define CONFIDENCE_MIDDLE_L1         (14) // 6 bits
  # define CONFIDENCE_MIDDLE_L2         (12) // 6 bits
  # define LAUNCH_MIDDLE_CONF           (8)
  
  /*****************************************************************************
   *                              LIMITS                                       *
   *****************************************************************************/
  # define MAX_HISTORY_IP               (8)
  # define MSHR_LIMIT                   (70)
  
  /*****************************************************************************
   *                              CONSTANT PARAMETERS                          *
   *****************************************************************************/
  # define BERTI_R                      (0x0)
  # define BERTI_L1                     (0x1)
  # define BERTI_L2                     (0x2)
  # define BERTI_L2R                    (0x3)

  /*****************************************************************************
   *                 BERTI-TDP: TAG-DIRECTED PREFETCHING (DESIGN.md)               *
   *****************************************************************************/
  // Sizes
  # define TDP_CHT_SIZE                  (128) // Capability History Table entries (FIFO)
  # define TDP_PDT_SIZE                  (64)  // Pointer Delta Table entries (fully associative, LRU)
  # define TDP_PDT_DELTAS                (4)   // deltas per PDT entry
  # define TDP_SIGHTQ                    (8)   // sighting queue entries (oldest dropped when full)
  # define TDP_SIGHT_PER_CYCLE           (2)   // sightings processed per cycle; 0 = unlimited

  // Confidence (Berti's epoch scheme, counted per sighting)
  # define TDP_EPOCH                     (16)  // sightings per epoch
  # define TDP_TH_L1                     (8)   // dereferences per epoch for an L1D-level delta
  # define TDP_TH_L2                     (4)   // ... for an L2-level delta
  # define TDP_MAX_LINE_DELTA            (7)   // |dereferenced line - cursor line| learned

  // Switches. The design is the value given; DESIGN.md section 5 has the measured alternatives.
  # define TDP_ENABLE                       (1)   // 0: Berti, bit for bit
  # define TDP_SIGHT_FILL                (1)   // sightings from lines arriving in L1D
  # define TDP_SIGHT_LOAD_HIT            (0)   // 1: also from the capability a demand load hit returns
  # define TDP_KEY_LEN                   (0)   // 1: key = hash(IP, pointed-to object length)
  # define TDP_BOUNDS                    (0)   // 1: TDP targets must overlap the pointed-to object
  # define TDP_NOLEARN                   (0)   // 1: no PDT; prefetch every sighted pointer's cursor line into L1D
  # define TDP_TIMELY                    (0)   // 1: credit a dereference miss only if its sighting was >= 1 fetch
                                              //    latency earlier (Berti's timeliness rule)
  # define TDP_PENDING                   (64)  // TDP_TIMELY: dereference misses waiting for their fill latency

  // Recursion. The defaults are the proposed recursion: TDP_RECUR 1, TDP_RECUR_KEY 5, TDP_RECUR_DECAY 2,
  // TDP_RECUR_DEPTH 7 (shared CHT). TDP_RECUR 0 gives the depth-1 design above, bit for bit.
  // Its controls without capability information: TDP_RECUR_KEY 6 (no bounds), TDP_RECUR_CTRL_FILL_NL 1 (no pointers).
  # define TDP_RECUR                     (1)   // which lines continue a chain: 0 none (depth 1);
                                              //   1 TDP's own L1D fills, scanned at the fill (no IP: key from TDP_RECUR_KEY);
                                              //   2 a TDP-prefetched line at its first demand hit, keyed by the hitting load's IP;
                                              //   3 both
  # define TDP_RECUR_KEY                 (5)   // key of a pointer found in a TDP fill (TDP_RECUR 1/3):
                                              //   0 hash(length of the object the line was fetched for, the line's offset in it,
                                              //     the slot) -- the field's identity, from the parent pointer's bounds (CHERI);
                                              //   1 control: the parent sighting's key (no bounds);
                                              //   2 control: hash(parent key, slot index in the line) (no bounds)
                                              //   3 hash(0's type, the parent sighting's key) (CHERI: type within a path)
                                              //   4 0's type key; while it has no confident delta, the parent sighting's deltas
                                              //   5 3's key, with 4's fallback to the parent sighting's deltas
                                              //   6 control for 5: 2's key (no bounds), with 4's fallback
  # define TDP_RECUR_DEPTH               (7)   // TDP_RECUR 1/3: TDP fills at this depth are not scanned (depth 1 = first hop);
                                              //   at most 7 (the metadata's depth field saturates at 7, so 8+ means no limit)
  # define TDP_RECUR_DECAY               (2)   // TDP_RECUR 1/3: 1 a hop's level comes from the chain's probability (product of the
                                              //   deltas' last-epoch rates); 0 each hop's own delta level, as for depth 1;
                                              //   2 own level for the sighting's own key, product only for fallback deltas
  # define TDP_RECUR_CREDIT              (0)   // n > 0: stop scanning TDP fills while n or more lines fetched by recursion (depth >= 2)
                                              //   sit in L1D unused; scanning resumes as they are used or evicted
  # define TDP_RECUR_FEEDBACK            (0)   // 1: learn the depth limit (2 .. TDP_RECUR_DEPTH, start 4) from how often the deepest
                                              //   level's fills are used, counted in 8 of the 64 L1D sets
  # define TDP_RECUR_HIT_OVERRIDE        (0)   // 1 (TDP_RECUR 3): a first-hit sighting is not dropped as a duplicate of a pointer
                                              //   that only the fill-time scan has recorded
  # define TDP_RECUR_CHT                 (0)   // 0: recursion's sightings share the CHT; n > 0: they go to their own n-entry FIFO
  # define TDP_RECUR_CTRL_NL             (0)   // n > 0: control for TDP_RECUR 2 without pointers: at the first demand hit on a
                                              //   TDP-prefetched line, prefetch its next n lines into L1D instead of scanning it
  # define TDP_RECUR_CTRL_FILL_NL        (0)   // n > 0: control for TDP_RECUR 1 without pointers: at each TDP fill, prefetch the
                                              //   filled line's next n lines (L1D if the MSHR allows) instead of scanning it
  // Measurement only (no effect on timing)
  # define TDP_CHAIN_STATS               (0)   // 1: dependent-miss chains, lead times and stopped chains
  # define TDP_OBSERVE_ONLY              (0)   // 1: Berti and TDP issue nothing (a no-prefetching run with TDP's statistics)
};
# endif