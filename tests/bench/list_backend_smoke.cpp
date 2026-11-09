// Focused list storage API parity test, without legacy tests that access
// the private contiguous items pointer directly.
extern int test_list_limit_main();
int main() { return test_list_limit_main(); }
