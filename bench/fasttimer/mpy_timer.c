#include <time.h>
#include <sys/time.h>

double 
timeit(void (*callback)(void), int times)
{
	struct timeval tv_before, tv_after;
	gettimeofday(&tv_before,NULL);
	for (int t = 0; t < times; ++t) {
		callback();
	}
	gettimeofday(&tv_after,NULL);

	double before = (double)tv_before.tv_sec + (double)tv_before.tv_usec / 1000000.0;
	double after = (double)tv_after.tv_sec + (double)tv_after.tv_usec / 1000000.0;

	return after-before;
}
