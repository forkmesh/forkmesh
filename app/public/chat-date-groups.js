function validDate(value) {
  const date = new Date(Number(value));
  return Number.isNaN(date.getTime()) ? new Date() : date;
}

export function localDateKey(value) {
  const date = validDate(value);
  const year = date.getFullYear();
  const month = String(date.getMonth() + 1).padStart(2, "0");
  const day = String(date.getDate()).padStart(2, "0");
  return `${year}-${month}-${day}`;
}

export function sameLocalDate(left, right) {
  return localDateKey(left) === localDateKey(right);
}

export function dateDividerLabel(value, nowValue = Date.now()) {
  const date = validDate(value);
  const now = validDate(nowValue);
  if (localDateKey(date.getTime()) === localDateKey(now.getTime())) return "Today";
  const yesterday = new Date(now.getFullYear(), now.getMonth(), now.getDate() - 1);
  if (localDateKey(date.getTime()) === localDateKey(yesterday.getTime())) {
    return "Yesterday";
  }
  return date.toLocaleDateString([], {
    weekday: "long",
    month: "long",
    day: "numeric",
    ...(date.getFullYear() === now.getFullYear() ? {} : { year: "numeric" }),
  });
}
