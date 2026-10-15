# Repository traffic

The Insights tab shows a 30-day chart of completed Git clone requests for a public repository. `GET /api/repo/<owner>/<repo>/traffic` returns one UTC-day bucket for each of the most recent 30 days, including zero-value days, plus the period total.

The endpoint reads `product_clone_daily`, which the routed HTTPS upload-pack path increments after serving a clone. It is public-repository data only; private repositories answer as not found. The chart is clone traffic, rather than a mixed request counter, so a Git client's multiple transport requests are not presented as separate clones.
