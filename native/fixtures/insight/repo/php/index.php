<?php
require_once 'lib.php';
use App\Models\User;

function handle($request) {
    if ($request && isset($request['id'])) {
        return find_user($request['id']);
    }
    return null;
}
