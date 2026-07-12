# Caeneus Go integration

This module contains the cgo client, native staging command, and tests. From
the repository root:

```bash
eval "$(go -C ext/go run ./cmd/caeneus-native --local)"
CGO_ENABLED=1 go -C ext/go test ./...
```

The staging command sets `CGO_LDFLAGS` for the local static archive. The C
header is [`../../include/caeneus.h`](../../include/caeneus.h).
