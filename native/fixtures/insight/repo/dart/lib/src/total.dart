import 'cart.dart';

int total(Cart cart) {
  var sum = 0;
  for (final item in cart.items) {
    sum += item > 0 ? item : 0;
  }
  return sum;
}
