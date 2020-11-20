#include <iostream>
#include <string>
#include <signal.h>
#include <sys/mman.h>

#include <thread>
#include <mutex>
#include <condition_variable>

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

#define MAX_PATH FILENAME_MAX
#define ENCLAVE_FILENAME "enclave.signed.so"

std::mutex m;
std::condition_variable cv;
bool ready = false;
bool processed = false;

#define PAUSE_THREAD1                               \
    do {                                            \
        {                                           \
            std::lock_guard <std::mutex> lk(m);     \
            ready = true;                           \
        }                                           \
        cv.notify_one();                            \
        {                                           \
            std::unique_lock <std::mutex> lk(m);    \
            cv.wait(lk, [] { return processed; });  \
        }                                           \
    } while(0)

#define PAUSE_THREAD2_1(lk)                         \
    do {                                            \
        cv.wait(lk, [] { return ready; });          \
    } while(0)

#define PAUSE_THREAD2_2(lk)                         \
    do {                                            \
        processed = true;                           \
        lk.unlock();                                \
        cv.notify_one();                            \
    } while(0)

int fault_fired = 0;
void *g_target_ptr = NULL;
uint64_t *pte_alias = NULL, pte_alias_unmapped = 0x0;

/* Called upon SIGSEGV caused by untrusted page tables. */
void fault_handler(int signal) {
    fault_fired++;
    printf("[fault_handler] %d\n", fault_fired);

//    PAUSE_THREAD1;

    /* remap enclave page, so abort page semantics apply and execution can continue. */
    *pte_alias = MARK_PRESENT(pte_alias_unmapped);
    ASSERT(!mprotect((void *) (((uint64_t) g_target_ptr) & ~PFN_MASK), 0x1000, PROT_READ | PROT_WRITE));
}

// ocalls for printing string (C++ ocalls)
void ocall_print_error(const char *str) {
    cerr << str << endl;
}

void ocall_print_string(const char *str) {
    cout << str;
}

void ocall_println_string(const char *str) {
    cout << str << endl;
}

void ocall_clear_addr_flag(void *target_ptr) {
    PAUSE_THREAD1;
    g_target_ptr = target_ptr;

    /* ensure a #PF on trigger accesses through the *alias* mapping */
    ASSERT(pte_alias = (uint64_t *) remap_page_table_level(target_ptr, PTE));
    pte_alias_unmapped = MARK_NOT_PRESENT(*pte_alias);
    ASSERT(!mprotect((void *) (((uint64_t) target_ptr) & ~PFN_MASK), 0x1000, PROT_NONE));
    *pte_alias = pte_alias_unmapped;
}

void thread1_func(sgx_enclave_id_t eid, const char *dbname) {
    sgx_status_t ret = SGX_ERROR_UNEXPECTED; // status flag for enclave calls

    // Open SQLite database
    cout << "[" << std::this_thread::get_id() << "] ecall_opendb" << endl;
    ret = ecall_opendb(eid, dbname);
    if (ret != SGX_SUCCESS) {
        cerr << "Error: Making an ecall_open()" << endl;
        return;
    }

    // Closing SQLite database inside enclave
    cout << "[" << std::this_thread::get_id() << "] ecall_closedb" << endl;
    ret = ecall_closedb(eid);
    if (ret != SGX_SUCCESS) {
        cerr << "Error: Making an ecall_closedb()" << endl;
        return;
    }
}

void thread2_func(sgx_enclave_id_t eid, const char *dbname) {
    std::unique_lock <std::mutex> lk(m);
    PAUSE_THREAD2_1(lk);

    sgx_status_t ret = SGX_ERROR_UNEXPECTED; // status flag for enclave calls

    cout << "Enter SQL statement to execute or 'quit' to exit: " << endl << "> ";
    string input;
    while (getline(cin, input)) {
        if (input == "quit") {
            break;
        }
        const char *sql = input.c_str();
        cout << "[" << std::this_thread::get_id() << "] ecall_execute_sql" << endl;
        ret = ecall_execute_sql(eid, sql);
        if (ret != SGX_SUCCESS) {
            cerr << "Error: Making an ecall_execute_sql()" << endl;
            return;
        }
        cout << "> ";
    }
    PAUSE_THREAD2_2(lk);
}

// Application entry
int main(int argc, char *argv[]) {
    const char *dbname = (argc != 2) ? "a.db" : argv[1];

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

    std::thread t1(thread1_func, eid, dbname);
    std::thread t2(thread2_func, eid, dbname);

    t1.join();
    printf("t1 return\n");
    t2.join();
    printf("t2 return\n");

    // Destroy the enclave
    sgx_destroy_enclave(eid);
    if (ret != SGX_SUCCESS) {
        cerr << "Error: destroying enclave" << endl;
        return -1;
    }

    cout << "Info: SQLite SGX enclave successfully returned." << endl;
    return 0;
}
