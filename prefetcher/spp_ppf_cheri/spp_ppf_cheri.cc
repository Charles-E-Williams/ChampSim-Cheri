#include <iostream>     // std::cout, std::endl
#include <iomanip>      // std::setw
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <cmath>
#include "spp_ppf_cheri.h"


void spp_ppf_cheri::PPF_Module::init(CACHE* cache)
{
    cache_ = cache;
	for(int a = 0; a < 30; a++)
		depth_track[a] = 0;

    ST.parent_ = this;
    PT.parent_ = this;
    FILTER.parent_ = this;
    GHR.parent_ = this;
    PERC.parent_ = this;

}

void spp_ppf_cheri::prefetcher_initialize() 
{
    module_.init(intern_);
}

uint32_t spp_ppf_cheri::prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
										uint32_t metadata_in)
{
    auto cap = intern_->get_authorizing_capability();
    if (!cheri::is_tag_valid(cap)) 
        return metadata_in;

    module_.do_prefetch(addr, ip,  cache_hit, useful_prefetch, type, metadata_in, cap);
    return metadata_in;
}

uint32_t spp_ppf_cheri::prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in)
{
    module_.handle_fill(addr,  set, way, prefetch, evicted_addr, metadata_in);
    return metadata_in;
}

void spp_ppf_cheri::PPF_Module::final_stats()
{


    if(SPP_PERC_WGHT) {
			
			ofstream myfile;
			char fname[] =  "perc_weights_0.csv";
			myfile.open(fname, std::ofstream::app);
			fmt::print("Printing all the perceptron weights to: {}\n",fname);

			std::string row = "base_addr,cache_line,page_addr,confidence^page_addr,curr_sig^sig_delta,ip_1^ip_2^ip_3,ip^depth,ip^sig_delta,confidence,\n"; 
			for (unsigned int i = 0; i < PERC_ENTRIES; i++) {
				//row = row + "Entry#: " + std::to_string(i) + ",";
				for (unsigned int j = 0; j < PERC_FEATURES; j++) {
					if (PERC.perc_touched[i][j]) {
						row = row + std::to_string(PERC.perc_weights[i][j]) + ",";
					}
					else {
						row = row + ",";
						if (PERC.perc_weights[i][j] != 0) {
							// Throw assertion if the weight is tagged as untouched and still non-zero 
							//cout << "I:" << i << "\tJ: "<< j << "\tWeight: " << PERC.perc_weights[i][j] << endl;
							//assert(0);
						}
					}
				}
				row = row + "\n";
			}
			myfile << row;
			myfile.close();	
    }

	int tot = 0;
    fmt::print("------------------\n");
    fmt::print("{} Depth Distribution\n", cache_->NAME);
    fmt::print("------------------\n");

	for(int a = 0; a < 30; a++){
        fmt::print("depth {}: {}\n",a, depth_track[a]);
		tot += depth_track[a];
	}
    fmt::print("Total: {}\n",tot);
	fmt::print("------------------\n");

}

void spp_ppf_cheri::prefetcher_final_stats()
{
    module_.final_stats();
}



