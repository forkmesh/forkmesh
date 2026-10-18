import 'package:shop_app/src/cart.dart';
import 'package:flutter/material.dart';
import 'src/total.dart';

void main() {
  final cart = Cart();
  if (cart.isEmpty && total(cart) == 0) {
    print('empty');
  }
}
