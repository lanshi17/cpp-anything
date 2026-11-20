// Package textpkg is the Go fixture for cpp-anything: five exported functions
// the wire can carry, plus constructs the analyzer must report as skipped.
package textpkg

import (
	"fmt"
	"strconv"
	"strings"
)

// WordCount counts whitespace-separated fields.
func WordCount(text string) int {
	return len(strings.Fields(text))
}

// Slash trims the text and replaces every ASCII space with a dash.
func Slash(text string) string {
	return strings.ReplaceAll(strings.TrimSpace(text), " ", "-")
}

// CountBetween counts non-overlapping occurrences of needle. An empty needle
// counts one more than the number of UTF-8 runes, as strings.Count does.
func CountBetween(text, needle string) int {
	return strings.Count(text, needle)
}

// Greater reports whether a is greater than b.
func Greater(a, b int) bool {
	return a > b
}

// MustParse trims the text and parses a base-10 int64, panicking on bad input.
func MustParse(text string) int {
	trimmed := strings.TrimSpace(text)
	value, err := strconv.ParseInt(trimmed, 10, 64)
	if err != nil {
		panic(fmt.Errorf("not a base-10 number: %q", text))
	}
	return int(value)
}

// Options is a struct: parameters of this type cannot cross the wire.
type Options struct {
	Width int
}

// Describe takes a struct parameter, so the analyzer reports it as skipped.
func Describe(options Options) string {
	return fmt.Sprintf("%d", options.Width)
}

// Sum is variadic, so the analyzer reports it as skipped.
func Sum(values ...int) int {
	total := 0
	for _, value := range values {
		total += value
	}
	return total
}

// Pair returns two values, so the analyzer reports it as skipped.
func Pair(text string) (int, error) {
	return len(text), nil
}

// Identity is generic, so the analyzer reports it as skipped.
func Identity[T any](value T) T {
	return value
}

func helper() int {
	return 0
}

// Size is a method, so the analyzer ignores it.
func (options Options) Size() int {
	return options.Width
}
