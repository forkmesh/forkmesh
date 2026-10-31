# Repository traffic

Part of [the App docs](README.md). The Insights tab
([repository-activity.md](repository-activity.md)) shows a 30-day chart of
completed Git clones of a public repository.
`GET /api/repo/<owner>/<repo>/traffic` returns one UTC-day bucket for each of
the last 30 days, zero days included, plus the period total.

The endpoint reads `product_clone_daily`, which the routed HTTPS upload-pack
path increments after serving a clone. Private repositories answer as not
found. Counting clones rather than requests keeps a Git client's several
transport requests from showing as several clones.
