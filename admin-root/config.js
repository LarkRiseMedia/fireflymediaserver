Event.observe(window,'load',init);
//###TODO
// * Disable/enable save/cancel, Add key and onchange listeners keep a note on changed items
// * create the path/file browser
// * better errormessage for non writable config
// * make tabs?
// * add warning if leaving page without saving
      
// Config isn't defined until after the Event.observe above
// I could have put it below Config = ... but I want all window.load events
// at the start of the file
var DEBUG = window.location.toString().match(/debug.*/);
if ('debug=validate' == DEBUG) {
  DEBUG = 'validate';
}
function init() {
  Config.init();
}
var ConfigXML = {
  config: {},
  getItem: function (id) {
    return this.config[id];
  },
  getAllItems: function () {
    return $H(this.config);
  },
  parseXML: function(xmlDoc) {
    $A(xmlDoc.getElementsByTagName('section')).each(function (section) {
      $A(section.getElementsByTagName('item')).each(function (item) {
        var returnItem = {};
        $A(item.attributes).each(function (attr) {
          returnItem[attr.name] = attr.value;  
        });
        Element.cleanWhitespace(item);
        $A(item.childNodes).each(function (node) {
          if (Element.textContent(node) == '') {
            return;
          }
          if ('options' == node.nodeName) {
            var options = [];
            $A(item.getElementsByTagName('option')).each(function (option) {
              options.push({value: option.getAttribute('value'),
                            label: Element.textContent(option)});
            });
            returnItem['options'] = options;     
          } else {
            returnItem[node.nodeName] = Element.textContent(node);
          }
          $A(node.attributes).each(function (attr) {
            returnItem[attr.name] = attr.value;  
          });
        });
        ConfigXML.config[returnItem.id] = returnItem;
      });
    });
  }
};
var ConfigInitialValues = {
  values: {},
  getValue: function (id) {
    return ConfigInitialValues.values[id];
  },
  parseXML: function (xmldoc) {
    // IE and w3c treat xmldoc differently make shore firstChild is firstchild of <config>
    if (xmldoc.childNodes[1] && xmldoc.childNodes[1].nodeName == 'config') {
      sections = $A(xmldoc.childNodes[1].childNodes);
    } else {  
      sections = $A(xmldoc.firstChild.childNodes);
    }
    sections.each(function (section) {
      var sectionName = section.nodeName;
      $A(section.childNodes).each(function (node) {
        if (node.firstChild && node.firstChild.hasChildNodes()) {
          var values = [];
          $A(node.childNodes).each(function (n) {
            values.push(Element.textContent(n));
          });
          ConfigInitialValues.values[sectionName+':'+node.nodeName] = values;
        } else {
          ConfigInitialValues.values[sectionName+':'+node.nodeName] = Element.textContent(node);
        }
      });
    });
  } 
};
var Config = {
  configPath: '',
  init: function () {
    new Ajax.Request('/config.xml',{method: 'get',onComplete: Config.storeConfigLayout});
  },
  storeConfigLayout: function (request) {
    // Need to store this until showConfig is run
    Config.tmpConfigXML = request.responseXML;
    ConfigXML.parseXML(request.responseXML);
    new Ajax.Request('/xml-rpc?method=stats',{method: 'get',onComplete: Config.updateStatus});
  },
  updateStatus: function (request) {
    Config.configPath = Element.textContent(request.responseXML.getElementsByTagName('config_path')[0]);
    Config.isWritable = Element.textContent(request.responseXML.getElementsByTagName('writable_config')[0]) == '1';
    new Ajax.Request('/xml-rpc?method=config',{method: 'get',onComplete: Config.showConfig});  
  },
  showConfig: function (request) {
    ConfigInitialValues.parseXML(request.responseXML);
    $A(Config.tmpConfigXML.getElementsByTagName('section')).each(function (section) {
      var head = document.createElement('div');
      head.className= 'naviheader';
      var sectionName = section.getAttribute('name');
      head.appendChild(document.createTextNode(sectionName));
      var body = document.createElement('div');
      body.className = 'navibox';
      if ('Server' == sectionName) {
        body.appendChild(Builder.node('span',{id:'config_path'},'Config File Location'));
        body.appendChild(document.createTextNode(Config.configPath));
        body.appendChild(Builder.node('br'));
        body.appendChild(Builder.node('div',{style: 'clear: both;'}));
      }
      $A(section.getElementsByTagName('item')).each(function (item) {
        body.appendChild(Config._buildItem(item.getAttribute('id')));
      });
      $('theform').appendChild(head);
      $('theform').appendChild(body);
    });
    // Won't be using the config.xml XML doc anymore get rid of it
    Config.tmpConfigXML = '';
    if (!Config.isWritable) {
      Effect.Appear('config_not_writable_warning');
    } else {
      // Create save and cancel buttons
//      var save = Builder.node('button',{id: 'button_save', disabled: 'disabled'},'Save');
      var save = Builder.node('button',{id: 'button_save'},'Save');
      Event.observe(save,'click',saveForm);
      var cancel = Builder.node('button',{id: 'button_cancel'},'Cancel');
      Event.observe(cancel,'click',cancelForm);
      var spacer = document.createTextNode('\u00a0\u00a0');
      var buttons = $('buttons');
      if (navigator.platform.toLowerCase().indexOf('mac') != -1) {
        // We're on mac
        buttons.appendChild(cancel);
        buttons.appendChild(spacer);
        buttons.appendChild(save);
      } else {
        //###TODO What about all them unix variants?
        buttons.appendChild(save);
        buttons.appendChild(spacer);
        buttons.appendChild(cancel);
      }
    }
    var advanced = Builder.node('a',{href: 'javascript://',id:'basic_config_button'},'Show basic config');
    Event.observe(advanced,'click',Config._showBasicConfig);  
    var basic = Builder.node('a',{href: 'javascript://',id:'advanced_config_button'},'Show advanced config');
    Event.observe(basic,'click',Config._showAdvancedConfig);  
    if (Cookie.getVar('show_advanced_config')) {
      basic.style.display = 'none';
    } else {
      advanced.style.display = 'none';
    }
    var div = $('toggle_basic_advanced');
    div.appendChild(advanced);
    div.appendChild(basic);
  },
  _buildItem: function(itemId) {
    var frag = document.createElement('div');
    var href;
    var item = ConfigXML.getItem(itemId);
    switch(item.type) {
      case 'text':
        if (item.multiple) {
          var values = ConfigInitialValues.getValue(itemId);
          if (!values || values.length === 0) {
            values = [''];
          }
//          var parentSpan = Builder.node('span');
          values.each(function (val,i) {
            var div = document.createElement('div');
            div.appendChild(BuildElement.input(itemId+i,itemId,
                                     item.name,
                                     val || item.default_value || '',
                                     item.size || 20,
                                     item.short_description,                                     
                                     ''));
            if (item.browse) {
              href = Builder.node('a',{href: 'javascript://'},'Browse');
              Event.observe(href,'click',Config._browse);          
              div.appendChild(href);
            }
            div.appendChild(document.createTextNode('\u00a0\u00a0'));
            href = Builder.node('a',{href: 'javascript://'},'Remove');
            Event.observe(href,'click',Config._removeItemEvent);
            div.appendChild(href);
            div.appendChild(Builder.node('br'));
            frag.appendChild(div);
          });
          // This is used by cancelForm to find out how
          // many options a multiple group has
//          frag.appendChild(parentSpan);
          href = Builder.node('a',{href:'javascript://',className:'addItemHref'},item.add_item_label);
          frag.appendChild(href);
          Event.observe(href,'click',Config._addItemEvent);
          frag.appendChild(Builder.node('div',{style:'clear: both'}));
        } else {
          frag.appendChild(BuildElement.input(itemId,itemId,
                                   item.name,
                                   ConfigInitialValues.getValue(itemId) || item.default_value || '',
                                   item.size || 20,
                                   item.short_description,                                     
                                   ''));
          if (item.browse) {
            href = Builder.node('a',{href: 'javascript://'},'Browse');
            Event.observe(href,'click',Config._browse);          
            frag.appendChild(href);
          }
          frag.appendChild(Builder.node('br'));
        }
        break;
      case 'select':
        frag.appendChild(BuildElement.select(itemId,
                                  item.name,
                                  item.options,
                                  ConfigInitialValues.getValue(itemId) || item.default_value,
                                  item.short_description));
        frag.appendChild(Builder.node('br'));
        break;
      default:
        alert('This should not happen (1)');
        break;
    }
    if (!Cookie.getVar('show_advanced_config') && item.advanced) {
      frag.style.display = 'none';
    }
    return frag;
  },
  _addItemEvent: function (e) {
    var div = Event.element(e).previousSibling;
    Config._addItem(div);      
  },
  _addItem: function(div) {
    var newSpan = div.cloneNode(true);
    var id = newSpan.getElementsByTagName('input')[0].id;
    var num = parseInt(id.match(/\d+$/));
    num++;
    id = id.replace(/\d+$/,'') + num;

    newSpan.getElementsByTagName('label')[0].setAttribute('for',id);
    newSpan.getElementsByTagName('input')[0].id = id;
    newSpan.getElementsByTagName('input')[0].value = '';
    newSpan.style.display = 'none';
    var hrefs = newSpan.getElementsByTagName('a');
    if ('Netscape' == navigator.appName) {
      // Firefox et al doesn't copy registered events on an element deep clone
      // Don't know if that is w3c or if IE has it right
      if (hrefs.length == 1) {
        Event.observe(hrefs[0],'click',Config._removeItemEvent);
      } else {
        Event.observe(hrefs[0],'click',Config._browse);          
        Event.observe(hrefs[1],'click',Config._removeItemEvent);        
      }
    }
    div.parentNode.insertBefore(newSpan,div.nextSibling);
    Effect.BlindDown(newSpan,{duration: 0.2});
  },
  _removeItemEvent: function (e) {
    var div = Event.element(e).parentNode;
    Config._removeItem(div);
  },
  _removeItem: function(div,noAnimation) {
    if (div.parentNode.getElementsByTagName('input').length > 1) {
      if (noAnimation) {
        // cancelForm uses a loop to delete elements, the loop can't wait
        // for Effect.BlindUp to finish
        Element.remove(div);
      } else {
        Effect.BlindUp(div,{duration: 0.2, afterFinish: function (){Element.remove(div);}});
      }
    } else {
      div.getElementsByTagName('input')[0].value='';
    }
  },
  _browse: function(e) {
    alert('Browse');
  },
  _showAdvancedConfig: function (e) {
    Element.toggle('advanced_config_button');
    Element.toggle('basic_config_button');
    Cookie.setVar('show_advanced_config','true',30);
    ConfigXML.getAllItems().each(function (item) {
      if (item.value.advanced) {
        var element = $(item.key);
        if (!element) {
          // Handle options with multiple values
          $A(document.getElementsByName(item.key)).each(function (el) {
            Effect.BlindDown(el.parentNode.parentNode);  
          });
        } else {
          Effect.BlindDown(element.parentNode);
        }
      }
    });
  },
  _showBasicConfig: function (e) {
    Element.toggle('advanced_config_button');
    Element.toggle('basic_config_button');
    Cookie.removeVar('show_advanced_config');
    ConfigXML.getAllItems().each(function (item) {
      if (item.value.advanced) {
        var element = $(item.key);
        if (!element) {
          // Handle options with multiple values
          $A(document.getElementsByName(item.key)).each(function (el) {
            Effect.BlindUp(el.parentNode.parentNode);  
          });
        } else {
          Effect.BlindUp($(item.key).parentNode);
        }
      }
    });
  }
};
var BuildElement = {
  input: function(id,name,displayName,value,size,short_description,long_description) {

    var frag = document.createDocumentFragment();
    var label = document.createElement('label');

    label.setAttribute('for',id);
    label.appendChild(document.createTextNode(displayName));
    frag.appendChild(label);
    if (Config.isWritable) {
      frag.appendChild(Builder.node('input',{id: id,name: name,className: 'text',
                                             value: value,size: size}));
    } else {
      frag.appendChild(Builder.node('input',{id: id,name: name,className: 'text',
                                             value: value,size: size, disabled: 'disabled'}));
    }
    frag.appendChild(document.createTextNode('\u00a0'));
    if (short_description) {                                              
      frag.appendChild(document.createTextNode(short_description));
    }

    return frag;
  },
  select: function(id,displayName,options,value,short_description,long_description) {
    var frag = document.createDocumentFragment();
    var label = document.createElement('label');
    label.setAttribute('for',id);
    label.appendChild(document.createTextNode(displayName));
    frag.appendChild(label);
    
    var select = Builder.node('select',{id: id,name: id,size: 1});
    if (!Config.isWritable) {
      select.disabled = 'disabled';
    }
    $A(options).each(function (option) {
      select.appendChild(Builder.node('option',{value: option.value},
                                                option.label));
    });
    select.value = value;
    frag.appendChild(select);
    frag.appendChild(document.createTextNode('\u00a0'));
    if (short_description) {
      frag.appendChild(document.createTextNode(short_description));
    }

    return frag;
  }
};

function saved(req) {
  if ('200' == Element.textContent(req.responseXML.getElementsByTagName('status')[0])) {
    alert('Saved');
  } else {
    alert("Couldn't save and if this weren't a beta I'd tell you why");
  }
}
function saveForm() {
  var postVars = [];
  var multiple = {};

  $A($('theform').getElementsByTagName('select')).each(function (select) {
    if (DEBUG) {
      debug(select.id,select.value);
    } else {
      postVars.push(Form.Element.serialize(select.id));
    }
  });

  $A($('theform').getElementsByTagName('input')).each(function (input) {
    if (ConfigXML.getItem(input.name).multiple) {
      if (multiple[input.name]) {
        multiple[input.name].push(encodeURIComponent(input.value));
      } else {
        multiple[input.name] = [input.value];
      }
    } else {
      if (DEBUG) {
        debug(input.id,input.value);
      } else {
        postVars.push(Form.Element.serialize(input.id));
      }
    }
  });

  $H(multiple).each(function (item) {
    if (DEBUG) {
      debug(item.key,item.value.join(','));
    } else {
      postVars.push(item.key + '=' + item.value.join(','));
    }
  });
  if (DEBUG) {
    return;
  }
  new Ajax.Request('/xml-rpc?method=updateconfig',
                   {method: 'post',
                parameters: postVars.join('&'),
                onComplete: saved});

  function debug(id,value) {
    var getArr = [];
    var getString;
    if ('validate' == DEBUG) {
      var a = id.split(':');
      getArr.push('section='+encodeURIComponent(a[0]));
      getArr.push('key='+encodeURIComponent(a[1]));
      getArr.push('value='+encodeURIComponent(value));
      getArr.push('verify_only=1');
      getString = '/xml-rpc?method=setconfig&' + getArr.join('&');

    } else {
      getString = '/xml-rpc?method=updateconfig&' + Form.Element.serialize(id);
    }
    var output = id + '=' + value;    
    new Ajax.Request(getString,
                     {method: 'get',
                  onComplete: function(req){
                         var errorString = Element.textContent(req.responseXML.getElementsByTagName('statusstring')[0]);
                         if (errorString != 'Success') {
                           console.log(output + ' => ' + errorString);
                         }
                  }});
  
      
  }
}
function cancelForm() {
  ConfigXML.getAllItems().each(function (item) {
    // this is from a hash $H hence use value
    item = item.value;
    if (item.multiple) {
      var values = ConfigInitialValues.getValue(item.id);
      if (!values || values.length === 0) {
        values = [''];
      }
      var initialValuesCount = values.length;
      var currentElements = document.getElementsByName(item.id);
var i=0;
      while (initialValuesCount < currentElements.length) {
        i++;
        if (i > 10) {
          alert('Getting dizzy; too many turns in this loop (silly errormessage 1)');
          return;
        }
        Config._removeItem(currentElements[0].parentNode,'noAnimation');
      }
      while (initialValuesCount > currentElements.length) {
        i++;
        if (i > 10) {
            alert('An important part came off (silly errormessage 2)');
            return;
        }
        Config._addItem(currentElements[currentElements.length-1].parentNode);
      }
      values.each(function (val,i){
        currentElements[i].value = val;
      });
    } else {
        //###TODO potential error a select without a default value
      $(item.id).value = ConfigInitialValues.getValue(item.id) || item.default_value || '';
    }
  });
  return;
}