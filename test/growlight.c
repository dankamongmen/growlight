#include <stdlib.h>
#include <CUnit/Basic.h>

static int
init_suite(void) {
	return 0;
}

static int
clean_suite(void) {
	return 0;
}

int main() {
	unsigned failures;
	CU_pSuite suite;
	if(CUE_SUCCESS != CU_initialize_registry()){
		fprintf(stderr, "Couldn't initialize CUnit (%d)\n", CU_get_error());
		exit(EXIT_FAILURE);
	}
	if((suite = CU_add_suite(__FILE__, init_suite, clean_suite)) == NULL){
		fprintf(stderr, "Couldn't add CUnit suite %s (%d)\n",
				__FILE__, CU_get_error());
		exit(EXIT_FAILURE);
	}
	CU_basic_set_mode(CU_BRM_VERBOSE);
	if(CU_basic_run_tests()){
		fprintf(stderr, "Error %d running CUnit tests\n", CU_get_error());
		exit(EXIT_FAILURE);
	}
	failures = CU_get_number_of_failures();
	if(failures){
		fprintf(stderr, "Returning %d after %u test failure\n",
				EXIT_FAILURE, failures);
		exit(EXIT_FAILURE);
	}
	CU_cleanup_registry();
	return CU_get_error();
}
