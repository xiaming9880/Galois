// Copyright 2019, University of Texas at Austin
// Authors: Xuhao Chen <cxh@utexas.edu>
#include "tc.h"
#include "../lonestarmine.h"

const char* name = "Triangle counting";
const char* desc = "Counting triangles in an undirected graph";
const char* url  = 0;

int main(int argc, char** argv) {
	LonestarMineStart(argc, argv, name, desc, url);
	AccType total;
	tc_gpu_solver(filename, total);
	std::cout << "\n\ttotal_num_triangles = " << total << "\n\n";
	return 0;
}

