<?php
function find_user($id) {
    foreach ([1, 2, 3] as $known) {
        if ($known === $id) {
            return $known;
        }
    }
    return null;
}
