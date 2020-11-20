#include <iostream>
#include <string>
#include <signal.h>
#include <sys/mman.h>

#include "sgx_urts.h"
#include "Enclave_u.h" // Headers for untrusted part (autogenerated by edger8r)

//#include "libsgxstep/apic.h"
#include "libsgxstep/pt.h"
#include "libsgxstep/sched.h"
#include "libsgxstep/enclave.h"
#include "libsgxstep/debug.h"
#include "libsgxstep/foreshadow.h"
#include "libsgxstep/cache.h"


using namespace std;

# define MAX_PATH FILENAME_MAX
# define ENCLAVE_FILENAME "enclave.signed.so"

int fault_fired = 0;
void *g_target_ptr = NULL;
uint64_t *pte_alias = NULL;
uint64_t pte_alias_unmapped = 0x0;

/* Called upon SIGSEGV caused by untrusted page tables. */
void fault_handler(int signal) {
    fault_fired++;
    printf("[fault_handler] %d\n", fault_fired);

    /* remap enclave page, so abort page semantics apply and execution can continue. */
    *pte_alias = MARK_PRESENT(pte_alias_unmapped);
    ASSERT(!mprotect((void *) (((uint64_t) g_target_ptr) & ~PFN_MASK), 0x1000, PROT_READ | PROT_WRITE));
}

// ocalls for printing string (C++ ocalls)
void ocall_print_error(const char *str){
    cerr << str << endl;
}

void ocall_print_string(const char *str){
    cout << str;
}

void ocall_println_string(const char *str){
    cout << str << endl;
}

void ocall_clear_addr_flag(void *target_ptr) {
    g_target_ptr = target_ptr;

    /* ensure a #PF on trigger accesses through the *alias* mapping */
    ASSERT(pte_alias = (uint64_t*)remap_page_table_level(target_ptr, PTE));
    pte_alias_unmapped = MARK_NOT_PRESENT(*pte_alias);
    ASSERT(!mprotect((void *) (((uint64_t) target_ptr) & ~PFN_MASK), 0x1000, PROT_NONE));
    *pte_alias = pte_alias_unmapped;
}

// Application entry
int main(int argc, char *argv[]){
    if ( argc != 2 ){
        cout << "Usage: " << argv[0] << " <database>" << endl;
        return -1;
    }
    const char* dbname = argv[1];

    ASSERT(signal(SIGSEGV, fault_handler) != SIG_ERR);

    sgx_enclave_id_t eid = 0;
    char token_path[MAX_PATH] = {'\0'};
    sgx_launch_token_t token = {0};
    sgx_status_t ret = SGX_ERROR_UNEXPECTED; // status flag for enclave calls
    int updated = 0;

    // Initialize the enclave
    ret = sgx_create_enclave(ENCLAVE_FILENAME, SGX_DEBUG_FLAG, &token, &updated, &eid, NULL);
    if (ret != SGX_SUCCESS) {
        cerr << "Error: creating enclave" << endl;
        return -1;
    }
    cout << "Info: SQLite SGX enclave successfully created." << endl;

    // Open SQLite database
    ret = ecall_opendb(eid, dbname);
    if (ret != SGX_SUCCESS) {
        cerr << "Error: Making an ecall_open()" << endl;
        return -1;
    }

    cout << "Enter SQL statement to execute or 'quit' to exit: " << endl;
    string input;
    cout << "> ";
    while(getline(cin, input)) {
        if (input == "quit"){
            break;
        }
        const char* sql = input.c_str();
        ret =  ecall_execute_sql(eid, sql);
        if (ret != SGX_SUCCESS) {
            cerr << "Error: Making an ecall_execute_sql()" << endl;
            return -1;
        }
        cout << "> ";
    }

    // Closing SQLite database inside enclave
    ret =  ecall_closedb(eid);
    if (ret != SGX_SUCCESS) {
        cerr << "Error: Making an ecall_closedb()" << endl;
        return -1;
    }

    // Destroy the enclave
    sgx_destroy_enclave(eid);
    if (ret != SGX_SUCCESS) {
        cerr << "Error: destroying enclave" << endl;
        return -1;
    }

    cout << "Info: SQLite SGX enclave successfully returned." << endl;
    return 0;
}