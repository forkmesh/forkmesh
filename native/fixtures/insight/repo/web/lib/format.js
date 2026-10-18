const math = require('./math');
const path = require('path');

function format(value) {
    return value ? String(math.add(value, 0)) : '';
}

module.exports = { format };
