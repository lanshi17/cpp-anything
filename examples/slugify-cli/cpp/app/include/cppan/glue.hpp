// The contract between the front door and a ported unit.
//
// Each `src/glue_<unit>.cpp` defines `bool handle_<unit>(int argc, char** argv,
// int* code)`: answer the command natively (fill `*code`, return true), or return
// false and the front door runs the original. There is no shared symbol here on
// purpose: handlers are per unit, declared where `routing.cpp` uses them.
#pragma once
