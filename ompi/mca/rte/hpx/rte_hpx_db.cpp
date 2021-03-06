#include "rte_hpx_db.h"


Node *current, *first, *secondcurrent, *secondfirst;

std::atomic<int> sum(0);

void rte_hpx_one()
{
  sum++;
}
HPX_PLAIN_ACTION(rte_hpx_one, rte_hpx_one_action);


int rte_hpx_vpid()
{
  return (int) hpx::get_locality_id();
}
HPX_PLAIN_ACTION(rte_hpx_vpid, rte_hpx_vpid_action);


int rte_hpx_num_localities()
{
  return hpx::get_initial_num_localities();
}


int rte_hpx_cpp_put(char* key, int keysize, char* val, int valsize)
{
  //std::cout << "rte_hpc_cpp_put: valsize = " << valsize << ", strlen(val) = " << strlen(val) << std::endl;  
  std::vector<char> vectorval (val, val + valsize);
  rte_hpx_put( std::string (key), vectorval );
  //std::cout << "rte_hpx_cpp_put: ((std::string)val).length() = " << ((std::string)val).length() << std::endl;
  return 0;
}


int rte_hpx_put(const std::string& key, const std::vector<char>& val)
{
  Node *temp;
  temp = new Node;
  
  temp->key = key;
  temp->value = val;
  //std::cout << "rte_hpx_put: temp->value.size() = " << temp->value->size() << std::endl;

  temp->next = NULL;
  temp->originator = hpx::get_locality_id();

  if (first == NULL){
    first = temp;
  }
  else{
    current = first;
    while (current->next != NULL){
      current = current->next;
    }
    current->next = temp;
  }
  return 0;
}


std::vector<char> rte_hpx_get( const std::string& key)
{
  //std::cout << "ompi>>mca>>rte>>hpx>>rte_hpx_get: " << "key = " << key << std::endl;
  static std::vector<char> empty;
  current = first;
  if (current == NULL){
	std::cout << "List is empty" << std::endl;
    return empty;
  }
  while (current != NULL){
    if (current->key == key){   //key found
      //std::cout << "key found: value = " << std::endl;
      return (current->value);
    }
    current = current->next;
  }
  std::cout << "rte_hpx_get: key not found - locality id: " <<  hpx::get_locality_id() << std::endl;
  return empty;
}
HPX_PLAIN_ACTION(rte_hpx_get, rte_hpx_get_action);



int rte_hpx_cpp_get_vpid_from_locality ( hpx::naming::id_type locality)
{
  return hpx::async<rte_hpx_vpid_action>(locality).get();
}


static hpx::naming::id_type* list;
void rte_hpx_local_map()
{
  int i = 0;
  std::vector<hpx::naming::id_type> localities =
    hpx::find_all_localities();
  
  list = new hpx::naming::id_type[localities.size()];

  BOOST_FOREACH(hpx::naming::id_type const& node, localities) {
    i = rte_hpx_cpp_get_vpid_from_locality (node); // to keep the order
    list[i] = node;
    std::cout << "list[" << i << "] = "<< list[i] << std::endl;
  }
  return;  
}


hpx::naming::id_type rte_hpx_cpp_get_locality_from_vpid ( int vpid )
{
  return list[vpid];
}


int rte_hpx_cpp_get( int vpid, char* key, char** value )
{
  hpx::naming::id_type const& node = rte_hpx_cpp_get_locality_from_vpid (vpid);
  std::string stringkey = key;
  int counter;
  
  *value = NULL;
  std::vector<char> temp_vector = hpx::async<rte_hpx_get_action>(node, stringkey).get();
  
  while (temp_vector.size() == 0) { //quick fix in case the key-value set is not there yet
    temp_vector = hpx::async<rte_hpx_get_action>(node, stringkey).get();
    counter++;
    if (counter == 5) break;
  }
  std::cout << "rte_hpx_get: locality id: " <<  hpx::get_locality_id()
            << ", temp_vector.size() = " << temp_vector.size() << std::endl;
  if (temp_vector.size() != 0) {
    char *temp_value = (char *) malloc (temp_vector.size());
    memcpy(temp_value, temp_vector.data(), temp_vector.size());
    *value = temp_value;
  }
  return (int) temp_vector.size();
}


void rte_hpx_barrier()
{
  std::vector<hpx::naming::id_type> localities =
    hpx::find_all_localities();
  
  std::vector<hpx::lcos::future<void> > futures;
  futures.reserve(localities.size());
 
  BOOST_FOREACH(hpx::naming::id_type const& node, localities) {
	futures.push_back(hpx::async<rte_hpx_one_action>(node));
  }
  hpx::wait_all(futures);
  
  while(sum != (int) localities.size()) {
    //std::cout << "Inside Barrier -  sum= " << sum 
    //	      << " ,number of localities = " << localities.size() << std::endl;
  }
  sum = 0;
  return;
}


int rte_hpx_cpp_printf(char* out)
{
  std::string str(out);
  hpx::cout << "RTE_HPX_CPP_PRINTF: " << str;
  return str.length();
}

