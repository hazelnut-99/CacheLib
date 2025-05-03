#include "SplayTree.h"
#include <initializer_list>

/*
           An implementation of top-down splaying with sizes
             D. Sleator <sleator@cs.cmu.edu>, January 1994.
Modified a little by Qingpeng Niu for tracing the global chunck library memory use. Just add a compute sum of size from search node to the right most node.
*/

namespace facebook {
namespace cachelib {

template <typename KeyType>
typename SplayTree<KeyType>::Node* SplayTree<KeyType>::splay(KeyType i, SplayTree<KeyType>::Node* t) {
    if (t == NULL) {
        return t;
    }

    Node N(i);
    Node* sides[2] = {&N, &N};
    Node* y;
    uint64_t sizes[2] = {0, 0};

    for (bool side; t->key != i && t->children[(side = i > t->key)] != NULL;) {
        if (i != t->children[side]->key && ((i > t->children[side]->key) == side)) {
            y = t->children[side]; /* rotate `side` */
            t->children[side] = y->children[1 - side];
            y->children[1 - side] = t;
            t->size = node_size(t->children[0]) + node_size(t->children[1]) + 1;
            t = y;
            if (t->children[side] == NULL) {
                break;
            }
        }
        sides[1 - side]->children[side] = t; /* link `side` */
        sides[1 - side] = t;
        t = t->children[side];
        sizes[1 - side] += 1 + node_size(sides[1 - side]->children[1 - side]);
    }

    sizes[0] += node_size(t->children[0]);
    sizes[1] += node_size(t->children[1]);
    t->size = sizes[0] + sizes[1] + 1;
    sides[0]->children[1] = sides[1]->children[0] = NULL;

    for (auto const& i : {0, 1}) {
        for (y = N.children[i]; y != NULL; y = y->children[i]) {
            y->size = sizes[1 - i];
            sizes[1 - i] -= 1 + node_size(y->children[1 - i]);
        }
    }
    for (auto const& i : {0, 1}) {
        sides[i]->children[1 - i] = t->children[i]; /* assemble */
        t->children[i] = N.children[1 - i];
    }

    return t;
}

template <typename KeyType>
void SplayTree<KeyType>::splay(KeyType i) {
    root = splay(i, root);
}

template <typename KeyType>
void SplayTree<KeyType>::insert(KeyType i) {
    if (root == NULL) {
        root = new Node(i);
    } else {
        auto t = splay(i, root);
        if (i == t->key) {
            root = t;
        } else {
            bool const side = i > t->key;
            root = new Node(i);
            root->children[side] = t->children[side];
            root->children[1 - side] = t;
            t->children[side] = NULL;
            t->size = 1 + SplayTree::node_size(t->children[1 - side]);
            root->size = 1 + SplayTree::node_size(root->children[0]) + SplayTree::node_size(root->children[1]);
        }
    }
}

template <typename KeyType>
void SplayTree<KeyType>::erase(KeyType i) {
    if (root != NULL) {
        Node* t = splay(i, root);
        if (i == t->key) {
            auto const old_root_size = root->size;
            if (t->children[0] == NULL) {
                root = t->children[1];
            } else {
                root = splay(i, t->children[0]);
                root->children[1] = t->children[1];
            }
            if (root != NULL) {
                root->size = old_root_size - 1;
            }
            delete t;
        }
    }
}

template <typename KeyType>
uint64_t SplayTree<KeyType>::greater_or_equal_to(KeyType key) const {
    uint64_t counter = 0;
    Node* t = root;
    while (t != NULL) {
        if (key == t->key) {
            counter += SplayTree::node_size(t->children[1]) + 1;
            break;
        } else if (key < t->key) {
            counter += SplayTree::node_size(t->children[1]) + 1;
            t = t->children[0];
        } else {
            t = t->children[1];
        }
    }
    return counter;
}

// Explicit template instantiations
template class SplayTree<uint32_t>;
template class SplayTree<uint64_t>;

} // namespace cachelib
} // namespace facebook