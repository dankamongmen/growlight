#include <stdlib.h>
#include <CUnit/Basic.h>

int main() {
	if(CUE_SUCCESS != CU_initialize_registry()){
		fprintf(stderr, "Couldn't initialize CUnit (%d)\n",
				CU_get_error());
		exit(EXIT_FAILURE);
	}
	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	return CU_get_error();
}
