//Neil McGlohon
//Rensselaer Polytechnic Institute
//October 5, 2018

#include <stdlib.h>
#include "ross.h"
#include "io-config.h"
#include <vector>
#include <map>


using namespace std;

//define two sufficiently large prime numbers for unique hashing
#define PRIME1 593441861	
#define PRIME2 613651349


/**
 * @brief hashes event to a unique identifier
 * 
 * Unique Identifier is based on 3 criteria:
 *  - Is it a cancel event? 0 or 1
 *  - its event_id (this is unique to the sending PE)
 *  - its send_pe (this is the peid of the sending PE)
 * 
 * These three criteria form a unique identifier for any event in
 * the simulation from start to finish. This is useful for reconnecting
 * events' pointers to other events by storing the event hashes.
 */
unsigned long long get_unique_event_hash(tw_event * e)
{
    tw_peid pe_id = e->send_pe;
    tw_eventid event_id = e->event_id;
    int is_cancel = e->state.cancel_q;

    unsigned long long the_hash = (pe_id * PRIME1 + event_id) * PRIME2 + is_cancel;

    return the_hash; 
}