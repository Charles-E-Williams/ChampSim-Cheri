# ifndef _BERTI_CHERI_PARAMETERS_H_
# define _BERTI_CHERI_PARAMETERS_H_

namespace berti_cheri_space
{
  /*****************************************************************************
   *                              SIZES                                        *
   *****************************************************************************/
  // BERTI
  # define BERTI_TABLE_SIZE             (64)
  # define BERTI_TABLE_DELTA_SIZE       (16)
  
  // HISTORY
  # define HISTORY_TABLE_SETS           (8)
  # define HISTORY_TABLE_WAYS           (16)

  // Hash Function
  # define ENTANGLING_HASH
 
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
  # define CONFIDENCE_MAX               (16)
  # define CONFIDENCE_INC               (1)
  # define CONFIDENCE_INIT              (1)
   
  # define CONFIDENCE_L1                (10)
  # define CONFIDENCE_L2                (8)
  # define CONFIDENCE_L2R               (6)
  
  # define CONFIDENCE_MIDDLE_L1         (14)
  # define CONFIDENCE_MIDDLE_L2         (12)
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
}
# endif