package main

import (
	"context"
	"os"

	modelregistry "github.com/aegis-mx/aegis-mx/control/model_registry"
)

func main() {
	os.Exit(modelregistry.RunCLI(context.Background(), os.Args[1:], os.Stdin, os.Stdout, os.Stderr))
}
