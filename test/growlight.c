#include <stdlib.h>
#include <CUnit/Basic.h>
#include "../src/growlight.h"

static int
init_suite(void) {
	return 0;
}

static int
clean_suite(void) {
	return 0;
}

static void
testGENPREFIX(void) {
	char buf[PREFIXSTRLEN + 1];

	genprefix(0, 1, buf, sizeof(buf), 1, 1000, '\0');
	CU_ASSERT_STRING_EQUAL(buf, "0");
	genprefix(0, 1, buf, sizeof(buf), 1, 1000, 'i');
	CU_ASSERT_STRING_EQUAL(buf, "0"); // no suffix on < mult
	genprefix(1, 1, buf, sizeof(buf), 1, 1000, '\0');
	CU_ASSERT_STRING_EQUAL(buf, "1");
	genprefix(1000, 1, buf, sizeof(buf), 1, 1000, '\0');
	CU_ASSERT_STRING_EQUAL(buf, "1K");
	genprefix(1000, 1, buf, sizeof(buf), 1, 1000, 'i');
	CU_ASSERT_STRING_EQUAL(buf, "1Ki");
	genprefix(1000, 1, buf, sizeof(buf), 1, 1024, 'i');
	CU_ASSERT_STRING_EQUAL(buf, "1000"); // FIXME should be 0.977Ki
	genprefix(1024, 1, buf, sizeof(buf), 1, 1024, 'i');
	CU_ASSERT_STRING_EQUAL(buf, "1Ki");
	genprefix(INTMAX_MAX, 1, buf, sizeof(buf), 1, 1000, '\0');
	// FIXME
	genprefix(UINTMAX_MAX, 1, buf, sizeof(buf), 1, 1000, '\0');
	// FIXME
	// FIXME
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
	CU_add_test(suite, "genprefix()", testGENPREFIX);
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
