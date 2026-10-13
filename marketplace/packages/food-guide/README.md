# Food Guide

A **Food Guide** section in the iPhone app that suggests breakfast, lunch and
dinner. It is a section you open, not an agent: nothing runs in the
background and nothing is sent anywhere.

## What it shows

- **Today's calorie budget.** The daily goal from the Calorie Counter, when it
  is installed and has one, else the standard 2,000 kcal daily value. What you
  have eaten today is the larger of the Calorie Counter's total and the
  dietary energy Apple Health holds for today. What is left is split across the
  meals still ahead (breakfast until 10:30, lunch until 15:00, dinner until
  21:00) in a 25 / 35 / 40 ratio; a meal whose time has passed is planned for
  tomorrow at its share of the full budget.
- **Your goal.** Lose weight, Keep my weight, Build muscle or Eat healthier.
  It changes how meals are ranked: weight loss favours protein and fiber and
  penalises going over the meal's budget hardest; muscle wants 30 g of
  protein or more; eat healthier favours fiber and potassium and penalises
  sodium and saturated fat.
- **What your diet needs.** With the Apple Health switch on, the phone reads
  the last 14 days of nutrition other apps logged (energy, protein, fiber,
  iron, calcium, vitamins C, D and B12, potassium, magnesium, folate, sodium,
  sugar and saturated fat). Days under 800 kcal and today are left out as
  incomplete. A nutrient averaging under 80% of its FDA daily value is
  **low**; sodium, sugar or saturated fat above it is **high**. Someone short
  of iron gets meals rich in iron first, each with a line such as "6.2 mg
  iron, 34% of the daily value". Without Apple Health, fiber and potassium
  (what most diets lack) steer the ranking.
- **Three ideas per meal**, each a real recipe published on the web, with its calories,
  time, the reasons it was picked, the groceries to buy at ALDI, and **Open
  recipe**. **Show other ideas** pages through the rest. No recipe appears
  in two meals of the same day.
- **Leave out**: vegetarian, vegan, dairy-free, gluten-free, no fish or
  shellfish, no nuts or peanuts.

## Where the data comes from

ALDI has no public product or nutrition API, so the catalog is bundled in the
app (`native/ios/ForkMeshCore/Sources/FMTools/FoodCatalog.swift`):

- **Groceries.** 48 ALDI US products, each name checked on aldi.us in
  October 2026 (store brands such as Millville, Friendly Farms, Simply Nature,
  Season's Choice, Dakota's Pride, Northern Catch, Kirkwood, Earth Grown,
  Southern Grove, Priano). Ingredients a recipe needs that were not checked
  are listed by their plain name (Cucumbers, Farro).
- **Nutrition.** Per 100 g, from USDA reference values for the generic food,
  so numbers are estimates; ALDI's labels are exact.
- **Recipes.** 31 recipes (11 per meal; one stir-fry serves lunch and
  dinner) from ALDI's own recipe pages (11), Cleveland Clinic (8), Heart
  Foundation (5), Diabetes Food Hub and Diabetes Australia (2 each), Healthy
  Food Guide, the Mayo Clinic Diet and UConn Husky Nutrition. Each page was
  opened and its title checked in October 2026. Ingredients are stored as
  grams per serving of catalog products (grains as cooked or dry weight, as
  the page gives them), so the app works out every nutrient itself; where a
  page states calories, the computed value is within about 25% of it.
  ALDI's recipe pages carry no breakfasts, so breakfast comes from the
  health sites.

## Privacy

Apple Health is read only, and only after the switch is on and iOS asks. The
nutrition history and the suggestions never leave the phone. The goal and the
foods left out are kept in the app's settings on the phone. The Calorie Counter
is read through its own account API for the goal and today's total.

Ideas, not medical advice: the screen says to talk to a doctor or dietitian
for a medical condition, pregnancy, an eating disorder, a child's diet or a
food allergy.

## History

Version 1.0.0 (2026-10-12) shipped Basil, a special agent, and a Suggest a
meal shortcut for a repository's `.forkmesh` folder. 2.0.0 replaced both with
this section: the user wanted suggestions to look at, not an agent to run.
2.1.0 (2026-10-13) declares `install.places: ["mobile"]`: Food Guide only
exists on iPhone, so the Market offers it for phones only.
