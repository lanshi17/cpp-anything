// Smoke test for the generated Go shadow binding. It imports the binding under
// the original package's name, as a caller would after swapping the import.
package main

import (
	"fmt"

	shadow "example.com/gopkg/cpp/textpkg/shadow"
)

func main() {
	fmt.Println("WordCount:", shadow.WordCount("Hello World"))
	fmt.Println("Slash:", shadow.Slash("  Mixed CASE  "))
	fmt.Println("CountBetween:", shadow.CountBetween("banana", "an"))
	fmt.Println("Greater:", shadow.Greater(2, 1))
	fmt.Println("MustParse:", shadow.MustParse("123"))

	func() {
		defer func() {
			if recovered := recover(); recovered != nil {
				fmt.Printf("MustParse bad input panicked: %T\n", recovered)
			}
		}()
		shadow.MustParse("not a number")
		fmt.Println("MustParse bad input did not panic")
	}()

	value, err := shadow.Call("Greater", 2, 1)
	fmt.Println("Call(Greater):", value, err)
}
