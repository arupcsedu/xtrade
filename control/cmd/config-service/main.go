package main

import (
	"context"
	"os"

	configservice "github.com/aegis-mx/aegis-mx/control/config_service"
)

func main() {
	os.Exit(configservice.RunCLI(context.Background(), os.Args[1:], os.Stdin, os.Stdout, os.Stderr))
}
