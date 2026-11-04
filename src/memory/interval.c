#include "memory/interval.h"
#include <stddef.h>

static int height(const struct jz_interval *node) { return node ? node->height : 0; }
static void update(struct jz_interval *node)
{
    int a = height(node->left), b = height(node->right);
    node->height = 1 + (a > b ? a : b);
}
static struct jz_interval *left(struct jz_interval *node)
{
    struct jz_interval *top = node->right;
    node->right = top->left; top->left = node;
    update(node); update(top);
    return top;
}
static struct jz_interval *right(struct jz_interval *node)
{
    struct jz_interval *top = node->left;
    node->left = top->right; top->right = node;
    update(node); update(top);
    return top;
}
static struct jz_interval *balance(struct jz_interval *node)
{
    update(node);
    int delta = height(node->left) - height(node->right);
    if (delta > 1) {
        if (height(node->left->left) < height(node->left->right))
            node->left = left(node->left);
        return right(node);
    }
    if (delta < -1) {
        if (height(node->right->right) < height(node->right->left))
            node->right = right(node->right);
        return left(node);
    }
    return node;
}
struct jz_interval *jz_interval_find(struct jz_interval *node, uintptr_t base)
{
    while (node && node->start != base)
        node = base < node->start ? node->left : node->right;
    return node;
}
struct jz_interval *jz_interval_overlap(struct jz_interval *node, uintptr_t start, uintptr_t end)
{
    while (node) {
        if (end <= node->start) node = node->left;
        else if (start >= node->end) node = node->right;
        else return node;
    }
    return NULL;
}
struct jz_interval *jz_interval_insert(struct jz_interval *root, struct jz_interval *node)
{
    if (!root) { node->height = 1; return node; }
    if (node->start < root->start) root->left = jz_interval_insert(root->left, node);
    else root->right = jz_interval_insert(root->right, node);
    return balance(root);
}
static struct jz_interval *detach_min(struct jz_interval *root, struct jz_interval **minimum)
{
    if (!root->left) { *minimum = root; return root->right; }
    root->left = detach_min(root->left, minimum);
    return balance(root);
}
struct jz_interval *jz_interval_remove(struct jz_interval *root, uintptr_t base)
{
    if (!root) return NULL;
    if (base < root->start) root->left = jz_interval_remove(root->left, base);
    else if (base > root->start) root->right = jz_interval_remove(root->right, base);
    else {
        if (!root->left) return root->right;
        if (!root->right) return root->left;
        /* Relink the successor; never copy its payload over a live record. */
        struct jz_interval *next = NULL;
        struct jz_interval *rest = detach_min(root->right, &next);
        next->left = root->left; next->right = rest;
        return balance(next);
    }
    return balance(root);
}
