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
};
# endif