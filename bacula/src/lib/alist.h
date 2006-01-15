/*
 *   Version $Id$
 *
 *  Kern Sibbald, June MMIII
 */
/*
   Copyright (C) 2003-2006 Kern Sibbald

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   version 2 as amended with additional clauses defined in the
   file LICENSE in the main source directory.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the 
   the file LICENSE for additional details.

 */


/*
 * There is a lot of extra casting here to work around the fact
 * that some compilers (Sun and Visual C++) do not accept
 * (void *) as an lvalue on the left side of an equal.
 *
 * Loop var through each member of list
 */
#define foreach_alist(var, list) \
    for((*((void **)&(var))=(void*)((list)->first())); \
         (var); \
         (*((void **)&(var))=(void*)((list)->next())))

#ifdef the_easy_way
#define foreach_alist(var, list) \
        for(((void*)(var))=(list)->first(); (var); ((void*)(var))=(list)->next()); )
#endif


/* Second arg of init */
enum {
  owned_by_alist = true,
  not_owned_by_alist = false
};

/*
 * Array list -- much like a simplified STL vector
 *   array of pointers to inserted items
 */
class alist : public SMARTALLOC {
   void **items;
   int num_items;
   int max_items;
   int num_grow;
   int cur_item;
   bool own_items;
   void grow_list(void);
public:
   alist(int num = 1, bool own=true);
   ~alist();
   void init(int num = 1, bool own=true);
   void append(void *item);
   void prepend(void *item);
   void *remove(int index);
   void *get(int index);
   bool empty() const;
   void *prev();
   void *next();
   void *first();
   void *last();
   void * operator [](int index) const;
   int size() const;
   void destroy();
   void grow(int num);

   /* Use it as a stack, pushing and poping from the end */
   void push(void *item) { append(item); };
   void *pop() { return remove(num_items-1); };
};

inline void * alist::operator [](int index) const {
   if (index < 0 || index >= num_items) {
      return NULL;
   }
   return items[index];
}

inline bool alist::empty() const
{
   /* Check for null pointer */
   return this ? num_items == 0 : true;
}

/*
 * This allows us to do explicit initialization,
 *   allowing us to mix C++ classes inside malloc'ed
 *   C structures. Define before called in constructor.
 */
inline void alist::init(int num, bool own) {
   items = NULL;
   num_items = 0;
   max_items = 0;
   num_grow = num;
   own_items = own;
}

/* Constructor */
inline alist::alist(int num, bool own) {
   init(num, own);
}

/* Destructor */
inline alist::~alist() {
   destroy();
}



/* Current size of list */
inline int alist::size() const
{
   /*
    * Check for null pointer, which allows test
    *  on size to succeed even if nothing put in
    *  alist.
    */
   return this ? num_items : 0;
}

/* How much to grow by each time */
inline void alist::grow(int num)
{
   num_grow = num;
}
