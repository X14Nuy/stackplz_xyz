// Code generated only as a local test stub; production builds overwrite this file.
package assets

import "fmt"

func Asset(name string) ([]byte, error) {
	return nil, fmt.Errorf("test asset %q is unavailable", name)
}

func RestoreAsset(path, name string) error {
	return fmt.Errorf("test asset %q is unavailable under %q", name, path)
}
