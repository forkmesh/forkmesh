local util = require("lua.util")

-- Entry point
local function run(n)
  if n > 0 and util.ok(n) then
    for i = 1, n do
      print(i)
    end
  end
end

return { run = run }
