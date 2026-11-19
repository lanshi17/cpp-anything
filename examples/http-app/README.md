# slugify server

The HTTP fixture for cpp-anything's server mode.

```sh
$ curl -s "http://127.0.0.1:{port}/health"
$ curl -s "http://127.0.0.1:{port}/slug?text=Hello%20World"
$ curl -s -X POST -H "Content-Type: application/json" -d '{"text":"Mixed CASE"}' "http://127.0.0.1:{port}/slug"
```
