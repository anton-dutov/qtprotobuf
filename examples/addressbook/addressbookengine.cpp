/*
 * MIT License
 *
 * Copyright (c) 2019 Alexey Edelev <semlanik@gmail.com>
 *
 * This file is part of qtprotobuf project https://git.semlanik.org/semlanik/qtprotobuf
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this
 * software and associated documentation files (the "Software"), to deal in the Software
 * without restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software, and
 * to permit persons to whom the Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies
 * or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
 * PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
 * FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "addressbookengine.h"
#include "addressbookclient.h"
#include <http2channel.h>

#include <QDebug>

using namespace qtprotobuf::examples;

AddressBookEngine::AddressBookEngine() : QObject()
  , m_client(new AddressBookClient)
  , m_contacts(new ContactsListModel({}, this))
{
    std::shared_ptr<qtprotobuf::AbstractChannel> channel(new qtprotobuf::Http2Channel("localhost", 65001));
    m_client->attachChannel(channel);
    m_client->getContacts(ListFrame(), this, [this](qtprotobuf::AsyncReply *reply) {
        m_contacts->reset(reply->read<Contacts>().list());
    });
}

void AddressBookEngine::addContact(qtprotobuf::examples::Contact *contact)
{
    m_client->addContact(*contact, this, [this](qtprotobuf::AsyncReply *reply) {
        m_contacts->reset(reply->read<Contacts>().list());
    });
}

AddressBookEngine::~AddressBookEngine()
{
    delete m_client;
}
